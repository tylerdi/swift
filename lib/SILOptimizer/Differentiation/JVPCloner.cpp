//===--- JVPCloner.cpp - JVP function generation --------------*- C++ -*---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines a helper class for generating JVP functions for automatic
// differentiation.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "differentiation"

#include "swift/SILOptimizer/Differentiation/JVPCloner.h"
#include "swift/SILOptimizer/Analysis/DifferentiableActivityAnalysis.h"
#include "swift/SILOptimizer/Differentiation/ADContext.h"
#include "swift/SILOptimizer/Differentiation/AdjointValue.h"
#include "swift/SILOptimizer/Differentiation/DifferentiationInvoker.h"
#include "swift/SILOptimizer/Differentiation/LinearMapInfo.h"
#include "swift/SILOptimizer/Differentiation/PullbackCloner.h"
#include "swift/SILOptimizer/Differentiation/Thunk.h"

#include "swift/SIL/TypeSubstCloner.h"
#include "swift/SILOptimizer/PassManager/PrettyStackTrace.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "llvm/ADT/DenseMap.h"

namespace swift {
namespace autodiff {

class JVPCloner::Implementation final
    : public TypeSubstCloner<JVPCloner::Implementation, SILOptFunctionBuilder> {
private:
  /// The global context.
  ADContext &context;

  /// The original function.
  SILFunction *const original;

  /// The witness.
  SILDifferentiabilityWitness *const witness;

  /// The JVP function.
  SILFunction *const jvp;

  llvm::BumpPtrAllocator allocator;

  /// The differentiation invoker.
  DifferentiationInvoker invoker;

  /// Info from activity analysis on the original function.
  const DifferentiableActivityInfo &activityInfo;

  /// The differential info.
  LinearMapInfo differentialInfo;

  bool errorOccurred = false;

  //--------------------------------------------------------------------------//
  // Differential generation related fields
  //--------------------------------------------------------------------------//

  /// The builder for the differential function.
  SILBuilder differentialBuilder;

  /// Mapping from original basic blocks to corresponding differential basic
  /// blocks.
  llvm::DenseMap<SILBasicBlock *, SILBasicBlock *> diffBBMap;

  /// Mapping from original basic blocks and original values to corresponding
  /// tangent values.
  llvm::DenseMap<SILValue, AdjointValue> tangentValueMap;

  /// Mapping from original basic blocks and original buffers to corresponding
  /// tangent buffers.
  llvm::DenseMap<std::pair<SILBasicBlock *, SILValue>, SILValue> bufferMap;

  /// Mapping from differential basic blocks to differential struct arguments.
  llvm::DenseMap<SILBasicBlock *, SILArgument *> differentialStructArguments;

  /// Mapping from differential struct field declarations to differential struct
  /// elements destructured from the linear map basic block argument. In the
  /// beginning of each differential basic block, the block's differential
  /// struct is destructured into the individual elements stored here.
  llvm::DenseMap<VarDecl *, SILValue> differentialStructElements;

  /// An auxiliary differential local allocation builder.
  SILBuilder diffLocalAllocBuilder;

  /// Stack buffers allocated for storing local tangent values.
  SmallVector<SILValue, 8> differentialLocalAllocations;

  /// Mapping from original blocks to differential values. Used to build
  /// differential struct instances.
  llvm::DenseMap<SILBasicBlock *, SmallVector<SILValue, 8>> differentialValues;

  //--------------------------------------------------------------------------//
  // Getters
  //--------------------------------------------------------------------------//

  ASTContext &getASTContext() const { return jvp->getASTContext(); }
  SILModule &getModule() const { return jvp->getModule(); }
  const SILAutoDiffIndices getIndices() const {
    return witness->getSILAutoDiffIndices();
  }
  SILBuilder &getDifferentialBuilder() { return differentialBuilder; }
  SILFunction &getDifferential() { return differentialBuilder.getFunction(); }
  SILArgument *getDifferentialStructArgument(SILBasicBlock *origBB) {
#ifndef NDEBUG
    auto *diffStruct = differentialStructArguments[origBB]
                           ->getType()
                           .getStructOrBoundGenericStruct();
    assert(diffStruct == differentialInfo.getLinearMapStruct(origBB));
#endif
    return differentialStructArguments[origBB];
  }

  //--------------------------------------------------------------------------//
  // Differential struct mapping
  //--------------------------------------------------------------------------//

  void initializeDifferentialStructElements(SILBasicBlock *origBB,
                                            SILInstructionResultArray values);

  SILValue getDifferentialStructElement(SILBasicBlock *origBB, VarDecl *field);

  //--------------------------------------------------------------------------//
  // General utilities
  //--------------------------------------------------------------------------//

  SILBasicBlock::iterator getNextDifferentialLocalAllocationInsertionPoint() {
    // If there are no local allocations, insert at the beginning of the tangent
    // entry.
    if (differentialLocalAllocations.empty())
      return getDifferential().getEntryBlock()->begin();
    // Otherwise, insert before the last local allocation. Inserting before
    // rather than after ensures that allocation and zero initialization
    // instructions are grouped together.
    auto lastLocalAlloc = differentialLocalAllocations.back();
    auto it = lastLocalAlloc->getDefiningInstruction()->getIterator();
    return it;
  }

  /// Get the lowered SIL type of the given AST type.
  SILType getLoweredType(Type type) {
    auto jvpGenSig = jvp->getLoweredFunctionType()->getSubstGenericSignature();
    Lowering::AbstractionPattern pattern(jvpGenSig,
                                         type->getCanonicalType(jvpGenSig));
    return jvp->getLoweredType(pattern, type);
  }

  /// Get the lowered SIL type of the given nominal type declaration.
  SILType getNominalDeclLoweredType(NominalTypeDecl *nominal) {
    auto nominalType =
        getOpASTType(nominal->getDeclaredInterfaceType()->getCanonicalType());
    return getLoweredType(nominalType);
  }

  /// Build a differential struct value for the original block corresponding to
  /// the given terminator.
  StructInst *buildDifferentialValueStructValue(TermInst *termInst) {
    assert(termInst->getFunction() == original);
    auto loc = termInst->getFunction()->getLocation();
    auto *origBB = termInst->getParent();
    auto *jvpBB = BBMap[origBB];
    assert(jvpBB && "Basic block mapping should exist");
    auto *diffStruct = differentialInfo.getLinearMapStruct(origBB);
    assert(diffStruct && "The differential struct should have been declared");
    auto structLoweredTy = getNominalDeclLoweredType(diffStruct);
    auto bbDifferentialValues = differentialValues[origBB];
    if (!origBB->isEntry()) {
      auto *enumArg = jvpBB->getArguments().back();
      bbDifferentialValues.insert(bbDifferentialValues.begin(), enumArg);
    }
    return getBuilder().createStruct(loc, structLoweredTy,
                                     bbDifferentialValues);
  }

  //--------------------------------------------------------------------------//
  // Tangent value factory methods
  //--------------------------------------------------------------------------//

  AdjointValue makeZeroTangentValue(SILType type) {
    return AdjointValue::createZero(allocator,
                                    remapSILTypeInDifferential(type));
  }

  AdjointValue makeConcreteTangentValue(SILValue value) {
    return AdjointValue::createConcrete(allocator, value);
  }

  //--------------------------------------------------------------------------//
  // Tangent materialization
  //--------------------------------------------------------------------------//

  void emitZeroIndirect(CanType type, SILValue bufferAccess, SILLocation loc) {
    auto builder = getDifferentialBuilder();
    auto tangentSpace = getTangentSpace(type);
    assert(tangentSpace && "No tangent space for this type");
    switch (tangentSpace->getKind()) {
    case TangentSpace::Kind::TangentVector:
      emitZeroIntoBuffer(builder, type, bufferAccess, loc);
      return;
    case TangentSpace::Kind::Tuple: {
      auto tupleType = tangentSpace->getTuple();
      SmallVector<SILValue, 8> zeroElements;
      for (unsigned i : range(tupleType->getNumElements())) {
        auto eltAddr = builder.createTupleElementAddr(loc, bufferAccess, i);
        emitZeroIndirect(tupleType->getElementType(i)->getCanonicalType(),
                         eltAddr, loc);
      }
      return;
    }
    }
  }

  SILValue emitZeroDirect(CanType type, SILLocation loc) {
    auto diffBuilder = getDifferentialBuilder();
    auto silType = getModule().Types.getLoweredLoadableType(
        type, TypeExpansionContext::minimal(), getModule());
    auto *buffer = diffBuilder.createAllocStack(loc, silType);
    emitZeroIndirect(type, buffer, loc);
    auto loaded = diffBuilder.emitLoadValueOperation(
        loc, buffer, LoadOwnershipQualifier::Take);
    diffBuilder.createDeallocStack(loc, buffer);
    return loaded;
  }

  SILValue materializeTangentDirect(AdjointValue val, SILLocation loc) {
    assert(val.getType().isObject());
    LLVM_DEBUG(getADDebugStream()
               << "Materializing tangents for " << val << '\n');
    switch (val.getKind()) {
    case AdjointValueKind::Zero: {
      auto zeroVal = emitZeroDirect(val.getSwiftType(), loc);
      return zeroVal;
    }
    case AdjointValueKind::Aggregate:
      llvm_unreachable(
          "Tuples and structs are not supported in forward mode yet.");
    case AdjointValueKind::Concrete:
      return val.getConcreteValue();
    }
    llvm_unreachable("Invalid adjoint value kind"); // silences MSVC C4715
  }

  SILValue materializeTangent(AdjointValue val, SILLocation loc) {
    if (val.isConcrete()) {
      LLVM_DEBUG(getADDebugStream()
                 << "Materializing tangent: Value is concrete.\n");
      return val.getConcreteValue();
    }
    LLVM_DEBUG(getADDebugStream() << "Materializing tangent: Value is "
                                     "non-concrete. Materializing directly.\n");
    return materializeTangentDirect(val, loc);
  }

  //--------------------------------------------------------------------------//
  // Tangent value mapping
  //--------------------------------------------------------------------------//

  /// Get the tangent for an original value. The given value must be in the
  /// original function.
  ///
  /// This method first tries to find an entry in `tangentValueMap`. If an entry
  /// doesn't exist, create a zero tangent.
  AdjointValue getTangentValue(SILValue originalValue) {
    assert(originalValue->getType().isObject());
    assert(originalValue->getFunction() == original);
    auto insertion = tangentValueMap.try_emplace(
        originalValue,
        makeZeroTangentValue(getRemappedTangentType(originalValue->getType())));
    return insertion.first->getSecond();
  }

  /// Map the tangent value to the given original value.
  void setTangentValue(SILBasicBlock *origBB, SILValue originalValue,
                       AdjointValue newTangentValue) {
#ifndef NDEBUG
    if (auto *defInst = originalValue->getDefiningInstruction()) {
      bool isTupleTypedApplyResult =
          isa<ApplyInst>(defInst) && originalValue->getType().is<TupleType>();
      assert(!isTupleTypedApplyResult &&
             "Should not set tangent value for tuple-typed result from `apply` "
             "instruction; use `destructure_tuple` on `apply` result and set "
             "tangent value for `destructure_tuple` results instead.");
    }
#endif
    assert(originalValue->getType().isObject());
    assert(newTangentValue.getType().isObject());
    assert(originalValue->getFunction() == original);
    LLVM_DEBUG(getADDebugStream() << "Adding tangent for " << originalValue);
    // The tangent value must be in the tangent space.
    assert(newTangentValue.getType() ==
           getRemappedTangentType(originalValue->getType()));
    auto insertion =
        tangentValueMap.try_emplace(originalValue, newTangentValue);
    (void)insertion;
    assert(insertion.second && "The tangent value should not already exist.");
  }

  //--------------------------------------------------------------------------//
  // Tangent buffer mapping
  //--------------------------------------------------------------------------//

  void setTangentBuffer(SILBasicBlock *origBB, SILValue originalBuffer,
                        SILValue tangentBuffer) {
    assert(originalBuffer->getType().isAddress());
    auto insertion =
        bufferMap.try_emplace({origBB, originalBuffer}, tangentBuffer);
    assert(insertion.second && "Tangent buffer already exists");
    (void)insertion;
  }

  SILValue &getTangentBuffer(SILBasicBlock *origBB, SILValue originalBuffer) {
    assert(originalBuffer->getType().isAddress());
    assert(originalBuffer->getFunction() == original);
    auto insertion =
        bufferMap.try_emplace({origBB, originalBuffer}, SILValue());
    assert(!insertion.second && "Tangent buffer should already exist");
    return insertion.first->getSecond();
  }

  //--------------------------------------------------------------------------//
  // Differential type calculations
  //--------------------------------------------------------------------------//

  /// Substitutes all replacement types of the given substitution map using the
  /// tangent function's substitution map.
  SubstitutionMap remapSubstitutionMapInDifferential(SubstitutionMap substMap) {
    return substMap.subst(getDifferential().getForwardingSubstitutionMap());
  }

  /// Remap any archetypes into the differential function's context.
  Type remapTypeInDifferential(Type ty) {
    if (ty->hasArchetype())
      return getDifferential().mapTypeIntoContext(ty->mapTypeOutOfContext());
    return getDifferential().mapTypeIntoContext(ty);
  }

  /// Remap any archetypes into the differential function's context.
  SILType remapSILTypeInDifferential(SILType ty) {
    if (ty.hasArchetype())
      return getDifferential().mapTypeIntoContext(ty.mapTypeOutOfContext());
    return getDifferential().mapTypeIntoContext(ty);
  }

  /// Find the tangent space of a given canonical type.
  Optional<TangentSpace> getTangentSpace(CanType type) {
    // Use witness generic signature to remap types.
    if (auto witnessGenSig = witness->getDerivativeGenericSignature())
      type = witnessGenSig->getCanonicalTypeInContext(type);
    return type->getAutoDiffTangentSpace(
        LookUpConformanceInModule(getModule().getSwiftModule()));
  }

  /// Assuming the given type conforms to `Differentiable` after remapping,
  /// returns the associated tangent space SIL type.
  SILType getRemappedTangentType(SILType type) {
    return SILType::getPrimitiveType(
        getTangentSpace(remapSILTypeInDifferential(type).getASTType())
            ->getCanonicalType(),
        type.getCategory());
  }

  /// Set up the differential function. This includes:
  /// - Creating all differential blocks.
  /// - Creating differential entry block arguments based on the function type.
  /// - Creating tangent value mapping for original/differential parameters.
  /// - Checking for unvaried result and emitting related warnings.
  void prepareForDifferentialGeneration();

public:
  explicit Implementation(ADContext &context, SILFunction *original,
                          SILDifferentiabilityWitness *witness,
                          SILFunction *jvp, DifferentiationInvoker invoker);

  static SILFunction *
  createEmptyDifferential(ADContext &context,
                          SILDifferentiabilityWitness *witness,
                          LinearMapInfo *linearMapInfo);

  /// Run JVP generation. Returns true on error.
  bool run();

  void postProcess(SILInstruction *orig, SILInstruction *cloned) {
    if (errorOccurred)
      return;
    SILClonerWithScopes::postProcess(orig, cloned);
  }

  /// Remap original basic blocks.
  SILBasicBlock *remapBasicBlock(SILBasicBlock *bb) {
    auto *jvpBB = BBMap[bb];
    return jvpBB;
  }

  /// General visitor for all instructions. If any error is emitted by previous
  /// visits, bail out.
  void visit(SILInstruction *inst) {
    if (errorOccurred)
      return;
    if (differentialInfo.shouldDifferentiateInstruction(inst)) {
      LLVM_DEBUG(getADDebugStream() << "JVPCloner visited:\n[ORIG]" << *inst);
#ifndef NDEBUG
      auto diffBuilder = getDifferentialBuilder();
      auto beforeInsertion = std::prev(diffBuilder.getInsertionPoint());
#endif
      TypeSubstCloner::visit(inst);
      LLVM_DEBUG({
        auto &s = llvm::dbgs() << "[TAN] Emitted in differential:\n";
        auto afterInsertion = diffBuilder.getInsertionPoint();
        for (auto it = ++beforeInsertion; it != afterInsertion; ++it)
          s << *it;
      });
    } else {
      TypeSubstCloner::visit(inst);
    }
  }

  void visitSILInstruction(SILInstruction *inst) {
    context.emitNondifferentiabilityError(
        inst, invoker, diag::autodiff_expression_not_differentiable_note);
    errorOccurred = true;
  }

  void visitInstructionsInBlock(SILBasicBlock *bb) {
    // Destructure the differential struct to get the elements.
    auto &diffBuilder = getDifferentialBuilder();
    auto diffLoc = getDifferential().getLocation();
    auto *diffBB = diffBBMap.lookup(bb);
    auto *mainDifferentialStruct = diffBB->getArguments().back();
    diffBuilder.setInsertionPoint(diffBB);
    auto *dsi =
        diffBuilder.createDestructureStruct(diffLoc, mainDifferentialStruct);
    initializeDifferentialStructElements(bb, dsi->getResults());
    TypeSubstCloner::visitInstructionsInBlock(bb);
  }

  // If an `apply` has active results or active inout parameters, replace it
  // with an `apply` of its JVP.
  void visitApplyInst(ApplyInst *ai) {
    // If the function should not be differentiated or its the array literal
    // initialization intrinsic, just do standard cloning.
    if (!differentialInfo.shouldDifferentiateApplySite(ai) ||
        ArraySemanticsCall(ai, semantics::ARRAY_UNINITIALIZED_INTRINSIC)) {
      LLVM_DEBUG(getADDebugStream() << "No active results:\n" << *ai << '\n');
      TypeSubstCloner::visitApplyInst(ai);
      return;
    }

    // Diagnose functions with active inout arguments.
    // TODO(TF-129): Support `inout` argument differentiation.
    for (auto inoutArg : ai->getInoutArguments()) {
      if (activityInfo.isActive(inoutArg, getIndices())) {
        context.emitNondifferentiabilityError(
            ai, invoker,
            diag::autodiff_cannot_differentiate_through_inout_arguments);
        errorOccurred = true;
        return;
      }
    }

    auto loc = ai->getLoc();
    auto &builder = getBuilder();
    auto origCallee = getOpValue(ai->getCallee());
    auto originalFnTy = origCallee->getType().castTo<SILFunctionType>();

    LLVM_DEBUG(getADDebugStream() << "JVP-transforming:\n" << *ai << '\n');

    // Get the minimal parameter and result indices required for differentiating
    // this `apply`.
    SmallVector<SILValue, 4> allResults;
    SmallVector<unsigned, 8> activeParamIndices;
    SmallVector<unsigned, 8> activeResultIndices;
    collectMinimalIndicesForFunctionCall(ai, getIndices(), activityInfo,
                                         allResults, activeParamIndices,
                                         activeResultIndices);
    assert(!activeParamIndices.empty() && "Parameter indices cannot be empty");
    assert(!activeResultIndices.empty() && "Result indices cannot be empty");
    LLVM_DEBUG(auto &s = getADDebugStream() << "Active indices: params={";
               llvm::interleave(
                   activeParamIndices.begin(), activeParamIndices.end(),
                   [&s](unsigned i) { s << i; }, [&s] { s << ", "; });
               s << "}, results={"; llvm::interleave(
                   activeResultIndices.begin(), activeResultIndices.end(),
                   [&s](unsigned i) { s << i; }, [&s] { s << ", "; });
               s << "}\n";);

    // Form expected indices.
    auto numResults =
        ai->getSubstCalleeType()->getNumResults() +
        ai->getSubstCalleeType()->getNumIndirectMutatingParameters();
    SILAutoDiffIndices indices(
        IndexSubset::get(getASTContext(),
                         ai->getArgumentsWithoutIndirectResults().size(),
                         activeParamIndices),
        IndexSubset::get(getASTContext(), numResults, activeResultIndices));

    // Emit the JVP.
    SILValue jvpValue;
    // If functionSource is a `@differentiable` function, just extract it.
    if (originalFnTy->isDifferentiable()) {
      auto paramIndices = originalFnTy->getDifferentiabilityParameterIndices();
      for (auto i : indices.parameters->getIndices()) {
        if (!paramIndices->contains(i)) {
          context.emitNondifferentiabilityError(
              origCallee, invoker,
              diag::
                  autodiff_function_noderivative_parameter_not_differentiable);
          errorOccurred = true;
          return;
        }
      }
      auto borrowedDiffFunc = builder.emitBeginBorrowOperation(loc, origCallee);
      jvpValue = builder.createDifferentiableFunctionExtract(
          loc, NormalDifferentiableFunctionTypeComponent::JVP,
          borrowedDiffFunc);
      jvpValue = builder.emitCopyValueOperation(loc, jvpValue);
    }

    // If JVP has not yet been found, emit an `differentiable_function`
    // instruction on the remapped  function operand and
    // an `differentiable_function_extract` instruction to get the JVP.
    // The `differentiable_function` instruction will be canonicalized during
    // the transform main loop.
    if (!jvpValue) {
      // FIXME: Handle indirect differentiation invokers. This may require some
      // redesign: currently, each original function + witness pair is mapped
      // only to one invoker.
      /*
       DifferentiationInvoker indirect(ai, attr);
       auto insertion =
           context.getInvokers().try_emplace({original, attr}, indirect);
       auto &invoker = insertion.first->getSecond();
       invoker = indirect;
       */

      // If the original `apply` instruction has a substitution map, then the
      // applied function is specialized.
      // In the JVP, specialization is also necessary for parity. The original
      // function operand is specialized with a remapped version of same
      // substitution map using an argument-less `partial_apply`.
      if (ai->getSubstitutionMap().empty()) {
        origCallee = builder.emitCopyValueOperation(loc, origCallee);
      } else {
        auto substMap = getOpSubstitutionMap(ai->getSubstitutionMap());
        auto jvpPartialApply = getBuilder().createPartialApply(
            ai->getLoc(), origCallee, substMap, {},
            ParameterConvention::Direct_Guaranteed);
        origCallee = jvpPartialApply;
      }

      // Check and diagnose non-differentiable original function type.
      auto diagnoseNondifferentiableOriginalFunctionType =
          [&](CanSILFunctionType origFnTy) {
            // Check and diagnose non-differentiable arguments.
            for (auto paramIndex : indices.parameters->getIndices()) {
              if (!originalFnTy->getParameters()[paramIndex]
                       .getSILStorageInterfaceType()
                       .isDifferentiable(getModule())) {
                context.emitNondifferentiabilityError(
                    ai->getArgumentsWithoutIndirectResults()[paramIndex],
                    invoker, diag::autodiff_nondifferentiable_argument);
                errorOccurred = true;
                return true;
              }
            }
            // Check and diagnose non-differentiable results.
            for (auto resultIndex : indices.results->getIndices()) {
              SILType remappedResultType;
              if (resultIndex >= originalFnTy->getNumResults()) {
                auto inoutArgIdx = resultIndex - originalFnTy->getNumResults();
                auto inoutArg =
                    *std::next(ai->getInoutArguments().begin(), inoutArgIdx);
                remappedResultType = inoutArg->getType();
              } else {
                remappedResultType = originalFnTy->getResults()[resultIndex]
                                         .getSILStorageInterfaceType();
              }
              if (!remappedResultType.isDifferentiable(getModule())) {
                context.emitNondifferentiabilityError(
                    origCallee, invoker,
                    diag::autodiff_nondifferentiable_result);
                errorOccurred = true;
                return true;
              }
            }
            return false;
          };
      if (diagnoseNondifferentiableOriginalFunctionType(originalFnTy))
        return;

      auto *diffFuncInst = context.createDifferentiableFunction(
          builder, loc, indices.parameters, indices.results, origCallee);

      // Record the `differentiable_function` instruction.
      context.getDifferentiableFunctionInstWorklist().push_back(diffFuncInst);

      auto borrowedADFunc = builder.emitBeginBorrowOperation(loc, diffFuncInst);
      auto extractedJVP = builder.createDifferentiableFunctionExtract(
          loc, NormalDifferentiableFunctionTypeComponent::JVP, borrowedADFunc);
      jvpValue = builder.emitCopyValueOperation(loc, extractedJVP);
      builder.emitEndBorrowOperation(loc, borrowedADFunc);
      builder.emitDestroyValueOperation(loc, diffFuncInst);
    }

    // Call the JVP using the original parameters.
    SmallVector<SILValue, 8> jvpArgs;
    auto jvpFnTy = getOpType(jvpValue->getType()).castTo<SILFunctionType>();
    auto numJVPArgs =
        jvpFnTy->getNumParameters() + jvpFnTy->getNumIndirectFormalResults();
    jvpArgs.reserve(numJVPArgs);
    // Collect substituted arguments.
    for (auto origArg : ai->getArguments())
      jvpArgs.push_back(getOpValue(origArg));
    assert(jvpArgs.size() == numJVPArgs);
    // Apply the JVP.
    // The JVP should be specialized, so no substitution map is necessary.
    auto *jvpCall = getBuilder().createApply(loc, jvpValue, SubstitutionMap(),
                                             jvpArgs, ai->isNonThrowing());
    LLVM_DEBUG(getADDebugStream() << "Applied jvp function\n" << *jvpCall);

    // Release the differentiable function.
    builder.emitDestroyValueOperation(loc, jvpValue);

    // Get the JVP results (original results and differential).
    SmallVector<SILValue, 8> jvpDirectResults;
    extractAllElements(jvpCall, builder, jvpDirectResults);
    auto originalDirectResults =
        ArrayRef<SILValue>(jvpDirectResults).drop_back(1);
    auto originalDirectResult =
        joinElements(originalDirectResults, getBuilder(), jvpCall->getLoc());

    mapValue(ai, originalDirectResult);

    // Some instructions that produce the callee may have been cloned.
    // If the original callee did not have any users beyond this `apply`,
    // recursively kill the cloned callee.
    if (auto *origCallee = cast_or_null<SingleValueInstruction>(
            ai->getCallee()->getDefiningInstruction()))
      if (origCallee->hasOneUse())
        recursivelyDeleteTriviallyDeadInstructions(
            getOpValue(origCallee)->getDefiningInstruction());

    // Add the differential function for when we create the struct we partially
    // apply to the differential we are generating.
    auto differential = jvpDirectResults.back();
    auto *differentialDecl = differentialInfo.lookUpLinearMapDecl(ai);
    auto originalDifferentialType =
        getOpType(differential->getType()).getAs<SILFunctionType>();
    auto loweredDifferentialType =
        getOpType(getLoweredType(differentialDecl->getInterfaceType()))
            .castTo<SILFunctionType>();
    // If actual differential type does not match lowered differential type,
    // reabstract the differential using a thunk.
    if (!loweredDifferentialType->isEqual(originalDifferentialType)) {
      SILOptFunctionBuilder fb(context.getTransform());
      differential = reabstractFunction(
          builder, fb, loc, differential, loweredDifferentialType,
          [this](SubstitutionMap subs) -> SubstitutionMap {
            return this->getOpSubstitutionMap(subs);
          });
    }
    differentialValues[ai->getParent()].push_back(differential);

    // Differential emission.
    emitTangentForApplyInst(ai, indices, originalDifferentialType);
  }

  void visitReturnInst(ReturnInst *ri) {
    auto loc = ri->getOperand().getLoc();
    auto *origExit = ri->getParent();
    auto &builder = getBuilder();
    auto *diffStructVal = buildDifferentialValueStructValue(ri);

    // Get the JVP value corresponding to the original functions's return value.
    auto *origRetInst = cast<ReturnInst>(origExit->getTerminator());
    auto origResult = getOpValue(origRetInst->getOperand());
    SmallVector<SILValue, 8> origResults;
    extractAllElements(origResult, builder, origResults);

    // Get and partially apply the differential.
    auto jvpGenericEnv = jvp->getGenericEnvironment();
    auto jvpSubstMap = jvpGenericEnv
                           ? jvpGenericEnv->getForwardingSubstitutionMap()
                           : jvp->getForwardingSubstitutionMap();
    auto *differentialRef = builder.createFunctionRef(loc, &getDifferential());
    auto *differentialPartialApply = builder.createPartialApply(
        loc, differentialRef, jvpSubstMap, {diffStructVal},
        ParameterConvention::Direct_Guaranteed);

    auto differentialType = jvp->getLoweredFunctionType()
                                ->getResults()
                                .back()
                                .getSILStorageInterfaceType();
    differentialType = differentialType.substGenericArgs(
        getModule(), jvpSubstMap, TypeExpansionContext::minimal());
    differentialType = differentialType.subst(getModule(), jvpSubstMap);
    auto differentialFnType = differentialType.castTo<SILFunctionType>();

    auto differentialSubstType =
        differentialPartialApply->getType().castTo<SILFunctionType>();
    SILValue differentialValue;
    if (differentialSubstType == differentialFnType) {
      differentialValue = differentialPartialApply;
    } else if (differentialSubstType
                   ->isABICompatibleWith(differentialFnType, *jvp)
                   .isCompatible()) {
      differentialValue = builder.createConvertFunction(
          loc, differentialPartialApply, differentialType,
          /*withoutActuallyEscaping*/ false);
    } else {
      // When `diag::autodiff_loadable_value_addressonly_tangent_unsupported`
      // applies, the return type may be ABI-incomaptible with the type of the
      // partially applied differential. In these cases, produce an undef and
      // rely on other code to emit a diagnostic.
      differentialValue = SILUndef::get(differentialType, *jvp);
    }

    // Return a tuple of the original result and differential.
    SmallVector<SILValue, 8> directResults;
    directResults.append(origResults.begin(), origResults.end());
    directResults.push_back(differentialValue);
    builder.createReturn(ri->getLoc(),
                         joinElements(directResults, builder, loc));
  }

  void visitBranchInst(BranchInst *bi) {
    llvm_unreachable("Unsupported SIL instruction.");
  }

  void visitCondBranchInst(CondBranchInst *cbi) {
    llvm_unreachable("Unsupported SIL instruction.");
  }

  void visitSwitchEnumInst(SwitchEnumInst *sei) {
    llvm_unreachable("Unsupported SIL instruction.");
  }

  void visitDifferentiableFunctionInst(DifferentiableFunctionInst *dfi) {
    // Clone `differentiable_function` from original to JVP, then add the cloned
    // instruction to the `differentiable_function` worklist.
    TypeSubstCloner::visitDifferentiableFunctionInst(dfi);
    auto *newDFI = cast<DifferentiableFunctionInst>(getOpValue(dfi));
    context.getDifferentiableFunctionInstWorklist().push_back(newDFI);
  }

  void visitLinearFunctionInst(LinearFunctionInst *lfi) {
    // Clone `linear_function` from original to JVP, then add the cloned
    // instruction to the `linear_function` worklist.
    TypeSubstCloner::visitLinearFunctionInst(lfi);
    auto *newLFI = cast<LinearFunctionInst>(getOpValue(lfi));
    context.getLinearFunctionInstWorklist().push_back(newLFI);
  }

  //--------------------------------------------------------------------------//
  // Tangent emission helpers
  //--------------------------------------------------------------------------//

#define CLONE_AND_EMIT_TANGENT(INST, ID)                                       \
  void visit##INST##Inst(INST##Inst *inst) {                                   \
    TypeSubstCloner::visit##INST##Inst(inst);                                  \
    if (differentialInfo.shouldDifferentiateInstruction(inst))                 \
      emitTangentFor##INST##Inst(inst);                                        \
  }                                                                            \
  void emitTangentFor##INST##Inst(INST##Inst *(ID))

  CLONE_AND_EMIT_TANGENT(BeginBorrow, bbi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto loc = bbi->getLoc();
    auto tanVal = materializeTangent(getTangentValue(bbi->getOperand()), loc);
    auto tanValBorrow = diffBuilder.emitBeginBorrowOperation(loc, tanVal);
    setTangentValue(bbi->getParent(), bbi,
                    makeConcreteTangentValue(tanValBorrow));
  }

  CLONE_AND_EMIT_TANGENT(EndBorrow, ebi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto loc = ebi->getLoc();
    auto tanVal = materializeTangent(getTangentValue(ebi->getOperand()), loc);
    diffBuilder.emitEndBorrowOperation(loc, tanVal);
  }

  CLONE_AND_EMIT_TANGENT(DestroyValue, dvi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto loc = dvi->getLoc();
    auto tanVal = materializeTangent(getTangentValue(dvi->getOperand()), loc);
    diffBuilder.emitDestroyValue(loc, tanVal);
  }

  CLONE_AND_EMIT_TANGENT(CopyValue, cvi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto tan = getTangentValue(cvi->getOperand());
    auto tanVal = materializeTangent(tan, cvi->getLoc());
    auto tanValCopy = diffBuilder.emitCopyValueOperation(cvi->getLoc(), tanVal);
    setTangentValue(cvi->getParent(), cvi,
                    makeConcreteTangentValue(tanValCopy));
  }

  /// Handle `load` instruction.
  ///   Original: y = load x
  ///    Tangent: tan[y] = load tan[x]
  CLONE_AND_EMIT_TANGENT(Load, li) {
    auto &diffBuilder = getDifferentialBuilder();
    auto *bb = li->getParent();
    auto loc = li->getLoc();
    auto tanBuf = getTangentBuffer(bb, li->getOperand());
    auto tanVal = diffBuilder.emitLoadValueOperation(
        loc, tanBuf, li->getOwnershipQualifier());
    setTangentValue(bb, li, makeConcreteTangentValue(tanVal));
  }

  /// Handle `load_borrow` instruction.
  ///   Original: y = load_borrow x
  ///    Tangent: tan[y] = load_borrow tan[x]
  CLONE_AND_EMIT_TANGENT(LoadBorrow, lbi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto *bb = lbi->getParent();
    auto loc = lbi->getLoc();
    auto tanBuf = getTangentBuffer(bb, lbi->getOperand());
    auto tanVal = diffBuilder.emitLoadBorrowOperation(loc, tanBuf);
    setTangentValue(bb, lbi, makeConcreteTangentValue(tanVal));
  }

  /// Handle `store` instruction in the differential.
  ///   Original: store x to y
  ///     Tangent: store tan[x] to tan[y]
  CLONE_AND_EMIT_TANGENT(Store, si) {
    auto &diffBuilder = getDifferentialBuilder();
    auto loc = si->getLoc();
    auto tanValSrc = materializeTangent(getTangentValue(si->getSrc()), loc);
    auto &tanValDest = getTangentBuffer(si->getParent(), si->getDest());
    diffBuilder.emitStoreValueOperation(loc, tanValSrc, tanValDest,
                                        si->getOwnershipQualifier());
  }

  /// Handle `store_borrow` instruction in the differential.
  ///   Original: store_borrow x to y
  ///    Tangent: store_borrow tan[x] to tan[y]
  CLONE_AND_EMIT_TANGENT(StoreBorrow, sbi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto loc = sbi->getLoc();
    auto tanValSrc = materializeTangent(getTangentValue(sbi->getSrc()), loc);
    auto &tanValDest = getTangentBuffer(sbi->getParent(), sbi->getDest());
    diffBuilder.createStoreBorrow(loc, tanValSrc, tanValDest);
  }

  /// Handle `copy_addr` instruction.
  ///   Original: copy_addr x to y
  ///    Tangent: copy_addr tan[x] to tan[y]
  CLONE_AND_EMIT_TANGENT(CopyAddr, cai) {
    auto diffBuilder = getDifferentialBuilder();
    auto loc = cai->getLoc();
    auto *bb = cai->getParent();
    auto &tanSrc = getTangentBuffer(bb, cai->getSrc());
    auto tanDest = getTangentBuffer(bb, cai->getDest());

    diffBuilder.createCopyAddr(loc, tanSrc, tanDest, cai->isTakeOfSrc(),
                               cai->isInitializationOfDest());
  }

  /// Handle `unconditional_checked_cast_addr` instruction.
  ///   Original: unconditional_checked_cast_addr $X in x to $Y in y
  ///    Tangent: unconditional_checked_cast_addr $X.Tan in tan[x]
  ///                                          to $Y.Tan in tan[y]
  CLONE_AND_EMIT_TANGENT(UnconditionalCheckedCastAddr, uccai) {
    auto diffBuilder = getDifferentialBuilder();
    auto loc = uccai->getLoc();
    auto *bb = uccai->getParent();
    auto &tanSrc = getTangentBuffer(bb, uccai->getSrc());
    auto tanDest = getTangentBuffer(bb, uccai->getDest());

    diffBuilder.createUnconditionalCheckedCastAddr(
        loc, tanSrc, tanSrc->getType().getASTType(), tanDest,
        tanDest->getType().getASTType());
  }

  /// Handle `begin_access` instruction (and do differentiability checks).
  ///   Original: y = begin_access x
  ///    Tangent: tan[y] = begin_access tan[x]
  CLONE_AND_EMIT_TANGENT(BeginAccess, bai) {
    // Check for non-differentiable writes.
    if (bai->getAccessKind() == SILAccessKind::Modify) {
      if (auto *gai = dyn_cast<GlobalAddrInst>(bai->getSource())) {
        context.emitNondifferentiabilityError(
            bai, invoker,
            diag::autodiff_cannot_differentiate_writes_to_global_variables);
        errorOccurred = true;
        return;
      }
      if (auto *pbi = dyn_cast<ProjectBoxInst>(bai->getSource())) {
        context.emitNondifferentiabilityError(
            bai, invoker,
            diag::autodiff_cannot_differentiate_writes_to_mutable_captures);
        errorOccurred = true;
        return;
      }
    }

    auto &diffBuilder = getDifferentialBuilder();
    auto *bb = bai->getParent();

    auto tanSrc = getTangentBuffer(bb, bai->getSource());
    auto *tanDest = diffBuilder.createBeginAccess(
        bai->getLoc(), tanSrc, bai->getAccessKind(), bai->getEnforcement(),
        bai->hasNoNestedConflict(), bai->isFromBuiltin());
    setTangentBuffer(bb, bai, tanDest);
  }

  /// Handle `end_access` instruction.
  ///   Original: begin_access x
  ///    Tangent: end_access tan[x]
  CLONE_AND_EMIT_TANGENT(EndAccess, eai) {
    auto &diffBuilder = getDifferentialBuilder();
    auto *bb = eai->getParent();
    auto loc = eai->getLoc();
    auto tanSrc = getTangentBuffer(bb, eai->getOperand());
    diffBuilder.createEndAccess(loc, tanSrc, eai->isAborting());
  }

  /// Handle `alloc_stack` instruction.
  ///   Original: y = alloc_stack $T
  ///    Tangent: tan[y] = alloc_stack $T.Tangent
  CLONE_AND_EMIT_TANGENT(AllocStack, asi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto *mappedAllocStackInst = diffBuilder.createAllocStack(
        asi->getLoc(), getRemappedTangentType(asi->getElementType()),
        asi->getVarInfo());
    bufferMap.try_emplace({asi->getParent(), asi}, mappedAllocStackInst);
  }

  /// Handle `dealloc_stack` instruction.
  ///   Original: dealloc_stack x
  ///    Tangent: dealloc_stack tan[x]
  CLONE_AND_EMIT_TANGENT(DeallocStack, dsi) {
    auto &diffBuilder = getDifferentialBuilder();
    auto tanBuf = getTangentBuffer(dsi->getParent(), dsi->getOperand());
    diffBuilder.createDeallocStack(dsi->getLoc(), tanBuf);
  }

  /// Handle `destroy_addr` instruction.
  ///   Original: destroy_addr x
  ///    Tangent: destroy_addr tan[x]
  CLONE_AND_EMIT_TANGENT(DestroyAddr, dai) {
    auto &diffBuilder = getDifferentialBuilder();
    auto tanBuf = getTangentBuffer(dai->getParent(), dai->getOperand());
    diffBuilder.createDestroyAddr(dai->getLoc(), tanBuf);
  }

  /// Handle `struct` instruction.
  ///   Original: y = struct $T (x0, x1, x2, ...)
  ///    Tangent: tan[y] = struct $T.Tangent (tan[x0], tan[x1], tan[x2], ...)
  CLONE_AND_EMIT_TANGENT(Struct, si) {
    auto &diffBuilder = getDifferentialBuilder();
    SmallVector<SILValue, 4> tangentElements;
    for (auto elem : si->getElements())
      tangentElements.push_back(getTangentValue(elem).getConcreteValue());
    auto tanExtract = diffBuilder.createStruct(
        si->getLoc(), getRemappedTangentType(si->getType()), tangentElements);
    setTangentValue(si->getParent(), si, makeConcreteTangentValue(tanExtract));
  }

  /// Handle `struct_extract` instruction.
  ///   Original: y = struct_extract x, #field
  ///    Tangent: tan[y] = struct_extract tan[x], #field'
  ///                                             ^~~~~~~
  ///                          field in tangent space corresponding to #field
  CLONE_AND_EMIT_TANGENT(StructExtract, sei) {
    assert(!sei->getField()->getAttrs().hasAttribute<NoDerivativeAttr>() &&
           "`struct_extract` with `@noDerivative` field should not be "
           "differentiated; activity analysis should not marked as varied.");
    auto diffBuilder = getDifferentialBuilder();
    auto loc = getValidLocation(sei);
    // Find the corresponding field in the tangent space.
    auto structType =
        remapSILTypeInDifferential(sei->getOperand()->getType()).getASTType();
    auto *tanField =
        getTangentStoredProperty(context, sei, structType, invoker);
    if (!tanField) {
      errorOccurred = true;
      return;
    }
    // Emit tangent `struct_extract`.
    auto tanStruct =
        materializeTangent(getTangentValue(sei->getOperand()), loc);
    auto tangentInst =
        diffBuilder.createStructExtract(loc, tanStruct, tanField);
    // Update tangent value mapping for `struct_extract` result.
    auto tangentResult = makeConcreteTangentValue(tangentInst);
    setTangentValue(sei->getParent(), sei, tangentResult);
  }

  /// Handle `struct_element_addr` instruction.
  ///   Original: y = struct_element_addr x, #field
  ///    Tangent: tan[y] = struct_element_addr tan[x], #field'
  ///                                                  ^~~~~~~
  ///                          field in tangent space corresponding to #field
  CLONE_AND_EMIT_TANGENT(StructElementAddr, seai) {
    assert(!seai->getField()->getAttrs().hasAttribute<NoDerivativeAttr>() &&
           "`struct_element_addr` with `@noDerivative` field should not be "
           "differentiated; activity analysis should not marked as varied.");
    auto diffBuilder = getDifferentialBuilder();
    auto *bb = seai->getParent();
    auto loc = getValidLocation(seai);
    // Find the corresponding field in the tangent space.
    auto structType =
        remapSILTypeInDifferential(seai->getOperand()->getType()).getASTType();
    auto *tanField =
        getTangentStoredProperty(context, seai, structType, invoker);
    if (!tanField) {
      errorOccurred = true;
      return;
    }
    // Emit tangent `struct_element_addr`.
    auto tanOperand = getTangentBuffer(bb, seai->getOperand());
    auto tangentInst =
        diffBuilder.createStructElementAddr(loc, tanOperand, tanField);
    // Update tangent buffer map for `struct_element_addr`.
    setTangentBuffer(bb, seai, tangentInst);
  }

  /// Handle `tuple` instruction.
  ///   Original: y = tuple (x0, x1, x2, ...)
  ///    Tangent: tan[y] = tuple (tan[x0], tan[x1], tan[x2], ...)
  ///                                                        ^~~
  ///                                      excluding non-differentiable elements
  CLONE_AND_EMIT_TANGENT(Tuple, ti) {
    auto diffBuilder = getDifferentialBuilder();
    // Get the tangents of all the tuple elements.
    SmallVector<SILValue, 8> tangentTupleElements;
    for (auto elem : ti->getElements()) {
      if (!getTangentSpace(elem->getType().getASTType()))
        continue;
      tangentTupleElements.push_back(
          materializeTangent(getTangentValue(elem), ti->getLoc()));
    }
    // Emit the instruction and add the tangent mapping.
    auto tanTuple =
        joinElements(tangentTupleElements, diffBuilder, ti->getLoc());
    setTangentValue(ti->getParent(), ti, makeConcreteTangentValue(tanTuple));
  }

  /// Handle `tuple_extract` instruction.
  ///   Original: y = tuple_extract x, <n>
  ///    Tangent: tan[y] = tuple_extract tan[x], <n'>
  ///                                            ^~~~
  ///                         tuple tangent space index corresponding to n
  CLONE_AND_EMIT_TANGENT(TupleExtract, tei) {
    auto &diffBuilder = getDifferentialBuilder();
    auto loc = tei->getLoc();
    auto origTupleTy = tei->getOperand()->getType().castTo<TupleType>();
    unsigned tanIndex = 0;
    for (unsigned i : range(tei->getFieldNo())) {
      if (getTangentSpace(
              origTupleTy->getElement(i).getType()->getCanonicalType()))
        ++tanIndex;
    }
    auto tanType = getRemappedTangentType(tei->getType());
    auto tanSource =
        materializeTangent(getTangentValue(tei->getOperand()), loc);
    SILValue tanBuf;
    // If the tangent buffer of the source does not have a tuple type, then
    // it must represent a "single element tuple type". Use it directly.
    if (!tanSource->getType().is<TupleType>()) {
      setTangentValue(tei->getParent(), tei,
                      makeConcreteTangentValue(tanSource));
    } else {
      tanBuf =
          diffBuilder.createTupleExtract(loc, tanSource, tanIndex, tanType);
      bufferMap.try_emplace({tei->getParent(), tei}, tanBuf);
    }
  }

  /// Handle `tuple_element_addr` instruction.
  ///   Original: y = tuple_element_addr x, <n>
  ///    Tangent: tan[y] = tuple_element_addr tan[x], <n'>
  ///                                                ^~~~
  ///                            tuple tangent space index corresponding to n
  CLONE_AND_EMIT_TANGENT(TupleElementAddr, teai) {
    auto &diffBuilder = getDifferentialBuilder();
    auto origTupleTy = teai->getOperand()->getType().castTo<TupleType>();
    unsigned tanIndex = 0;
    for (unsigned i : range(teai->getFieldNo())) {
      if (getTangentSpace(
              origTupleTy->getElement(i).getType()->getCanonicalType()))
        ++tanIndex;
    }
    auto tanType = getRemappedTangentType(teai->getType());
    auto tanSource = getTangentBuffer(teai->getParent(), teai->getOperand());
    SILValue tanBuf;
    // If the tangent buffer of the source does not have a tuple type, then
    // it must represent a "single element tuple type". Use it directly.
    if (!tanSource->getType().is<TupleType>()) {
      tanBuf = tanSource;
    } else {
      tanBuf = diffBuilder.createTupleElementAddr(teai->getLoc(), tanSource,
                                                  tanIndex, tanType);
    }
    bufferMap.try_emplace({teai->getParent(), teai}, tanBuf);
  }

  /// Handle `destructure_tuple` instruction.
  ///   Original: (y0, y1, ...)  = destructure_tuple x, <n>
  ///    Tangent: (tan[y0], tan[y1], ...) = destructure_tuple tan[x], <n'>
  ///                                                                 ^~~~
  ///                              tuple tangent space index corresponding to n
  CLONE_AND_EMIT_TANGENT(DestructureTuple, dti) {
    assert(llvm::any_of(dti->getResults(),
                        [&](SILValue elt) {
                          return activityInfo.isActive(elt, getIndices());
                        }) &&
           "`destructure_tuple` should have at least one active result");

    auto &diffBuilder = getDifferentialBuilder();
    auto *bb = dti->getParent();
    auto loc = dti->getLoc();

    auto tanTuple = materializeTangent(getTangentValue(dti->getOperand()), loc);
    SmallVector<SILValue, 4> tanElts;
    if (tanTuple->getType().is<TupleType>()) {
      auto *tanDti = diffBuilder.createDestructureTuple(loc, tanTuple);
      tanElts.append(tanDti->getResults().begin(), tanDti->getResults().end());
    } else {
      tanElts.push_back(tanTuple);
    }
    unsigned tanIdx = 0;
    for (auto i : range(dti->getNumResults())) {
      auto origElt = dti->getResult(i);
      if (!getTangentSpace(origElt->getType().getASTType()))
        continue;
      setTangentValue(bb, origElt, makeConcreteTangentValue(tanElts[tanIdx++]));
    }
  }

#undef CLONE_AND_EMIT_TANGENT

  /// Handle `apply` instruction, given:
  /// - The minimal indices for differentiating the `apply`.
  /// - The original non-reabstracted differential type.
  ///
  ///   Original: y = apply f(x0, x1, ...)
  ///    Tangent: tan[y] = apply diff_f(tan[x0], tan[x1], ...)
  void emitTangentForApplyInst(ApplyInst *ai, SILAutoDiffIndices applyIndices,
                               CanSILFunctionType originalDifferentialType) {
    assert(differentialInfo.shouldDifferentiateApplySite(ai));
    auto *bb = ai->getParent();
    auto loc = ai->getLoc();
    auto &diffBuilder = getDifferentialBuilder();

    // Get the differential value.
    auto *field = differentialInfo.lookUpLinearMapDecl(ai);
    assert(field);
    SILValue differential = getDifferentialStructElement(bb, field);
    auto differentialType = remapSILTypeInDifferential(differential->getType())
                                .castTo<SILFunctionType>();

    // Get the differential arguments.
    SmallVector<SILValue, 8> diffArgs;

    for (auto indRes : ai->getIndirectSILResults())
      diffArgs.push_back(getTangentBuffer(bb, indRes));

    auto paramArgs = ai->getArgumentsWithoutIndirectResults();
    // Get the tangent value of the original arguments.
    for (auto i : indices(paramArgs)) {
      auto origArg = paramArgs[i];
      // If the argument is not active:
      // - Skip the element, if it is not differentiable.
      // - Otherwise, add a zero value to that location.
      if (!activityInfo.isActive(origArg, getIndices())) {
        auto origCalleeType = ai->getSubstCalleeType();
        if (!origCalleeType->isDifferentiable())
          continue;
        auto actualOrigCalleeIndices =
            origCalleeType->getDifferentiabilityParameterIndices();
        if (actualOrigCalleeIndices->contains(i)) {
          SILValue tanParam;
          if (origArg->getType().isObject()) {
            tanParam = emitZeroDirect(
                getRemappedTangentType(origArg->getType()).getASTType(), loc);
            diffArgs.push_back(tanParam);
          } else {
            tanParam = diffBuilder.createAllocStack(
                loc, getRemappedTangentType(origArg->getType()));
            emitZeroIndirect(
                getRemappedTangentType(origArg->getType()).getASTType(),
                tanParam, loc);
          }
        }
      }
      // Otherwise, if the argument is active, handle the argument normally by
      // getting its tangent value.
      else {
        SILValue tanParam;
        if (origArg->getType().isObject()) {
          tanParam = materializeTangent(getTangentValue(origArg), loc);
        } else {
          tanParam = getTangentBuffer(ai->getParent(), origArg);
        }
        diffArgs.push_back(tanParam);
        if (errorOccurred)
          return;
      }
    }

    // If callee differential was reabstracted in JVP, reabstract the callee
    // differential.
    if (!differentialType->isEqual(originalDifferentialType)) {
      SILOptFunctionBuilder fb(context.getTransform());
      differential = reabstractFunction(
          diffBuilder, fb, loc, differential, originalDifferentialType,
          [this](SubstitutionMap subs) -> SubstitutionMap {
            return this->getOpSubstitutionMap(subs);
          });
    }

    // Call the differential.
    auto *differentialCall =
        diffBuilder.createApply(loc, differential, SubstitutionMap(), diffArgs,
                                /*isNonThrowing*/ false);
    diffBuilder.emitDestroyValueOperation(loc, differential);

    // Get the original `apply` results.
    SmallVector<SILValue, 8> origDirectResults;
    forEachApplyDirectResult(ai, [&](SILValue directResult) {
      origDirectResults.push_back(directResult);
    });
    SmallVector<SILValue, 8> origAllResults;
    collectAllActualResultsInTypeOrder(ai, origDirectResults, origAllResults);

    // Get the callee differential `apply` results.
    SmallVector<SILValue, 8> differentialDirectResults;
    extractAllElements(differentialCall, getDifferentialBuilder(),
                       differentialDirectResults);
    SmallVector<SILValue, 8> differentialAllResults;
    collectAllActualResultsInTypeOrder(
        differentialCall, differentialDirectResults, differentialAllResults);
    assert(applyIndices.results->getNumIndices() ==
           differentialAllResults.size());

    // Set tangent values for original `apply` results.
    unsigned differentialResultIndex = 0;
    for (auto resultIndex : applyIndices.results->getIndices()) {
      auto origResult = origAllResults[resultIndex];
      auto differentialResult =
          differentialAllResults[differentialResultIndex++];
      if (origResult->getType().isObject()) {
        if (!origResult->getType().is<TupleType>()) {
          setTangentValue(bb, origResult,
                          makeConcreteTangentValue(differentialResult));
        } else if (auto *dti = getSingleDestructureTupleUser(ai)) {
          bool notSetValue = true;
          for (auto result : dti->getResults()) {
            if (activityInfo.isActive(result, getIndices())) {
              assert(notSetValue &&
                     "This was incorrectly set, should only have one active "
                     "result from the tuple.");
              notSetValue = false;
              setTangentValue(bb, result,
                              makeConcreteTangentValue(differentialResult));
            }
          }
        }
      }
    }
  }

  /// Generate a `return` instruction in the current differential basic block.
  void emitReturnInstForDifferential() {
    auto &differential = getDifferential();
    auto diffLoc = differential.getLocation();
    auto &diffBuilder = getDifferentialBuilder();

    // Collect original results.
    SmallVector<SILValue, 2> originalResults;
    collectAllDirectResultsInTypeOrder(*original, originalResults);
    // Collect differential return elements.
    SmallVector<SILValue, 8> retElts;
    // for (auto origResult : originalResults) {
    for (auto i : range(originalResults.size())) {
      auto origResult = originalResults[i];
      if (!getIndices().results->contains(i))
        continue;
      auto tanVal = materializeTangent(getTangentValue(origResult), diffLoc);
      retElts.push_back(tanVal);
    }

    diffBuilder.createReturn(diffLoc,
                             joinElements(retElts, diffBuilder, diffLoc));
  }
};

//--------------------------------------------------------------------------//
// Initialization
//--------------------------------------------------------------------------//

/// Initialization helper function.
///
/// Returns the substitution map used for type remapping.
static SubstitutionMap getSubstitutionMap(SILFunction *original,
                                          SILFunction *jvp) {
  auto substMap = original->getForwardingSubstitutionMap();
  if (auto *jvpGenEnv = jvp->getGenericEnvironment()) {
    auto jvpSubstMap = jvpGenEnv->getForwardingSubstitutionMap();
    substMap = SubstitutionMap::get(
        jvpGenEnv->getGenericSignature(), QuerySubstitutionMap{jvpSubstMap},
        LookUpConformanceInSubstitutionMap(jvpSubstMap));
  }
  return substMap;
}

/// Initialization helper function.
///
/// Returns the activity info for the given original function, autodiff indices,
/// and JVP generic signature.
static const DifferentiableActivityInfo &
getActivityInfo(ADContext &context, SILFunction *original,
                SILAutoDiffIndices indices, SILFunction *jvp) {
  // Get activity info of the original function.
  auto &passManager = context.getPassManager();
  auto *activityAnalysis =
      passManager.getAnalysis<DifferentiableActivityAnalysis>();
  auto &activityCollection = *activityAnalysis->get(original);
  auto &activityInfo = activityCollection.getActivityInfo(
      jvp->getLoweredFunctionType()->getSubstGenericSignature(),
      AutoDiffDerivativeFunctionKind::JVP);
  LLVM_DEBUG(activityInfo.dump(indices, getADDebugStream()));
  return activityInfo;
}

JVPCloner::Implementation::Implementation(ADContext &context,
                                          SILFunction *original,
                                          SILDifferentiabilityWitness *witness,
                                          SILFunction *jvp,
                                          DifferentiationInvoker invoker)
    : TypeSubstCloner(*jvp, *original, getSubstitutionMap(original, jvp)),
      context(context), original(original), witness(witness), jvp(jvp),
      invoker(invoker),
      activityInfo(getActivityInfo(context, original,
                                   witness->getSILAutoDiffIndices(), jvp)),
      differentialInfo(context, AutoDiffLinearMapKind::Differential, original,
                       jvp, witness->getSILAutoDiffIndices(), activityInfo),
      differentialBuilder(SILBuilder(
          *createEmptyDifferential(context, witness, &differentialInfo))),
      diffLocalAllocBuilder(getDifferential()) {
  // Create empty differential function.
  context.recordGeneratedFunction(&getDifferential());
}

JVPCloner::JVPCloner(ADContext &context, SILFunction *original,
                     SILDifferentiabilityWitness *witness, SILFunction *jvp,
                     DifferentiationInvoker invoker)
    : impl(*new Implementation(context, original, witness, jvp, invoker)) {}

JVPCloner::~JVPCloner() { delete &impl; }

//--------------------------------------------------------------------------//
// Differential struct mapping
//--------------------------------------------------------------------------//

void JVPCloner::Implementation::initializeDifferentialStructElements(
    SILBasicBlock *origBB, SILInstructionResultArray values) {
  auto *diffStructDecl = differentialInfo.getLinearMapStruct(origBB);
  assert(diffStructDecl->getStoredProperties().size() == values.size() &&
         "The number of differential struct fields must equal the number of "
         "differential struct element values");
  for (auto pair : llvm::zip(diffStructDecl->getStoredProperties(), values)) {
    assert(std::get<1>(pair).getOwnershipKind() !=
               ValueOwnershipKind::Guaranteed &&
           "Differential struct elements must be @owned");
    auto insertion = differentialStructElements.insert(
        {std::get<0>(pair), std::get<1>(pair)});
    (void)insertion;
    assert(insertion.second &&
           "A differential struct element mapping already exists!");
  }
}

SILValue
JVPCloner::Implementation::getDifferentialStructElement(SILBasicBlock *origBB,
                                                        VarDecl *field) {
  assert(differentialInfo.getLinearMapStruct(origBB) ==
         cast<StructDecl>(field->getDeclContext()));
  assert(differentialStructElements.count(field) &&
         "Differential struct element for this field does not exist!");
  return differentialStructElements.lookup(field);
}

//--------------------------------------------------------------------------//
// Tangent emission helpers
//--------------------------------------------------------------------------//

void JVPCloner::Implementation::prepareForDifferentialGeneration() {
  // Create differential blocks and arguments.
  auto &differential = getDifferential();
  auto *origEntry = original->getEntryBlock();
  for (auto &origBB : *original) {
    auto *diffBB = differential.createBasicBlock();
    diffBBMap.insert({&origBB, diffBB});
    // If the BB is the original entry, then the differential block that we
    // just created must be the differential function's entry. Create
    // differential entry arguments and continue.
    if (&origBB == origEntry) {
      assert(diffBB->isEntry());
      createEntryArguments(&differential);
      auto *lastArg = diffBB->getArguments().back();
#ifndef NDEBUG
      auto diffStructLoweredType = remapSILTypeInDifferential(
          differentialInfo.getLinearMapStructLoweredType(&origBB));
      assert(lastArg->getType() == diffStructLoweredType);
#endif
      differentialStructArguments[&origBB] = lastArg;
    }

    LLVM_DEBUG({
      auto &s = getADDebugStream()
                << "Original bb" + std::to_string(origBB.getDebugID())
                << ": To differentiate or not to differentiate?\n";
      for (auto &inst : origBB) {
        s << (differentialInfo.shouldDifferentiateInstruction(&inst) ? "[x] "
                                                                     : "[ ] ")
          << inst;
      }
    });
  }

  assert(diffBBMap.size() == 1 &&
         "Can only currently handle single basic block functions");

  // The differential function has type:
  // (arg0', ..., argn', entry_df_struct) -> result'.
  auto diffParamArgs =
      differential.getArgumentsWithoutIndirectResults().drop_back();
  assert(diffParamArgs.size() ==
         witness->getSILAutoDiffIndices().parameters->getNumIndices());
  auto origParamArgs = original->getArgumentsWithoutIndirectResults();

  // TODO(TF-788): Re-enable non-varied result warning.
  /*
  // Check if result is not varied.
  SmallVector<SILValue, 8> origFormalResults;
  collectAllFormalResultsInTypeOrder(*original, origFormalResults);
   std::get<0>(pair);
  for (auto resultIndex : getIndices().results->getIndices()) {
    auto origResult = origFormalResults[resultIndex];
    // Emit warning if original result is not varied, because it will always
    // have a zero derivative.
    if (!activityInfo.isVaried(origResult, getIndices().parameters)) {
      // Emit fixit if original result has a valid source location.
      auto startLoc = origResult.getLoc().getStartSourceLoc();
      auto endLoc = origResult.getLoc().getEndSourceLoc();
      if (startLoc.isValid() && endLoc.isValid()) {
        context.diagnose(startLoc, diag::autodiff_nonvaried_result_fixit)
            .fixItInsert(startLoc, "withoutDerivative(at:")
            .fixItInsertAfter(endLoc, ")");
      }
    }
  }
  */

  // Initialize tangent mapping for parameters.
  auto diffParamsIt = getIndices().parameters->begin();
  for (auto index : range(diffParamArgs.size())) {
    auto *diffArg = diffParamArgs[index];
    auto *origArg = origParamArgs[*diffParamsIt];
    ++diffParamsIt;
    if (diffArg->getType().isAddress()) {
      setTangentBuffer(origEntry, origArg, diffArg);
    } else {
      setTangentValue(origEntry, origArg, makeConcreteTangentValue(diffArg));
    }
    LLVM_DEBUG(getADDebugStream()
               << "Assigned parameter " << *diffArg
               << " as the tangent of original result " << *origArg);
  }

  // Initialize tangent mapping for indirect results.
  auto origIndResults = original->getIndirectResults();
  auto diffIndResults = differential.getIndirectResults();
#ifndef NDEBUG
  unsigned numInoutParameters = llvm::count_if(
      original->getLoweredFunctionType()->getParameters(),
      [](SILParameterInfo paramInfo) { return paramInfo.isIndirectInOut(); });
  assert(origIndResults.size() + numInoutParameters == diffIndResults.size());
#endif
  for (auto &origBB : *original)
    for (auto i : indices(origIndResults))
      setTangentBuffer(&origBB, origIndResults[i], diffIndResults[i]);
}

/*static*/ SILFunction *JVPCloner::Implementation::createEmptyDifferential(
    ADContext &context, SILDifferentiabilityWitness *witness,
    LinearMapInfo *linearMapInfo) {
  auto &module = context.getModule();
  auto *original = witness->getOriginalFunction();
  auto *jvp = witness->getJVP();
  auto origTy = original->getLoweredFunctionType();
  // Get witness generic signature for remapping types.
  // Witness generic signature may have more requirements than JVP generic
  // signature: when witness generic signature has same-type requirements
  // binding all generic parameters to concrete types, JVP function type uses
  // all the concrete types and JVP generic signature is null.
  CanGenericSignature witnessCanGenSig;
  if (auto witnessGenSig = witness->getDerivativeGenericSignature())
    witnessCanGenSig = witnessGenSig->getCanonicalSignature();
  auto lookupConformance = LookUpConformanceInModule(module.getSwiftModule());

  // Parameters of the differential are:
  // - the tangent values of the wrt parameters.
  // - the differential struct for the original entry.
  // Result of the differential is in the tangent space of the original
  // result.
  SmallVector<SILParameterInfo, 8> dfParams;
  SmallVector<SILResultInfo, 8> dfResults;
  auto origParams = origTy->getParameters();
  auto indices = witness->getSILAutoDiffIndices();

  // Add differential results.
  Optional<SILParameterInfo> inoutDiffParam = None;
  for (auto origParam : origTy->getParameters()) {
    if (!origParam.isIndirectInOut())
      continue;
    inoutDiffParam = origParam;
  }

  if (inoutDiffParam) {
    dfResults.push_back(
        SILResultInfo(inoutDiffParam->getInterfaceType()
                          ->getAutoDiffTangentSpace(lookupConformance)
                          ->getType()
                          ->getCanonicalType(witnessCanGenSig),
                      ResultConvention::Indirect));
  } else {
    for (auto resultIndex : indices.results->getIndices()) {
      auto origResult = origTy->getResults()[resultIndex];
      origResult = origResult.getWithInterfaceType(
          origResult.getInterfaceType()->getCanonicalType(witnessCanGenSig));
      dfResults.push_back(
          SILResultInfo(origResult.getInterfaceType()
                            ->getAutoDiffTangentSpace(lookupConformance)
                            ->getType()
                            ->getCanonicalType(witnessCanGenSig),
                        origResult.getConvention()));
    }
  }

  // Add differential parameters for the requested wrt parameters.
  for (auto i : indices.parameters->getIndices()) {
    auto origParam = origParams[i];
    origParam = origParam.getWithInterfaceType(
        origParam.getInterfaceType()->getCanonicalType(witnessCanGenSig));
    dfParams.push_back(
        SILParameterInfo(origParam.getInterfaceType()
                             ->getAutoDiffTangentSpace(lookupConformance)
                             ->getType()
                             ->getCanonicalType(witnessCanGenSig),
                         origParam.getConvention()));
  }

  // Accept a differential struct in the differential parameter list. This is
  // the returned differential's closure context.
  auto *origEntry = original->getEntryBlock();
  auto *dfStruct = linearMapInfo->getLinearMapStruct(origEntry);
  auto dfStructType =
      dfStruct->getDeclaredInterfaceType()->getCanonicalType(witnessCanGenSig);
  dfParams.push_back({dfStructType, ParameterConvention::Direct_Owned});

  Mangle::ASTMangler mangler;
  auto diffName =
      original->getASTContext()
          .getIdentifier(mangler.mangleAutoDiffLinearMapHelper(
              original->getName(), AutoDiffLinearMapKind::Differential,
              witness->getConfig()))
          .str();
  // Set differential generic signature equal to JVP generic signature.
  // Do not use witness generic signature, which may have same-type requirements
  // binding all generic parameters to concrete types.
  auto diffGenericSig =
      jvp->getLoweredFunctionType()->getSubstGenericSignature();
  auto *diffGenericEnv =
      diffGenericSig ? diffGenericSig->getGenericEnvironment() : nullptr;
  auto diffType = SILFunctionType::get(
      diffGenericSig, origTy->getExtInfo(), origTy->getCoroutineKind(),
      origTy->getCalleeConvention(), dfParams, {}, dfResults, None,
      origTy->getPatternSubstitutions(), origTy->getInvocationSubstitutions(),
      original->getASTContext());

  SILOptFunctionBuilder fb(context.getTransform());
  auto linkage = jvp->isSerialized() ? SILLinkage::Public : SILLinkage::Hidden;
  auto *differential = fb.createFunction(
      linkage, diffName, diffType, diffGenericEnv, original->getLocation(),
      original->isBare(), IsNotTransparent, jvp->isSerialized(),
      original->isDynamicallyReplaceable());
  differential->setDebugScope(
      new (module) SILDebugScope(original->getLocation(), differential));

  return differential;
}

bool JVPCloner::Implementation::run() {
  PrettyStackTraceSILFunction trace("generating JVP and differential for",
                                    original);
  LLVM_DEBUG(getADDebugStream() << "Cloning original @" << original->getName()
                                << " to jvp @" << jvp->getName() << '\n');
  // Create JVP and differential entry and arguments.
  auto *entry = jvp->createBasicBlock();
  createEntryArguments(jvp);
  prepareForDifferentialGeneration();
  // Clone.
  SmallVector<SILValue, 4> entryArgs(entry->getArguments().begin(),
                                     entry->getArguments().end());
  cloneFunctionBody(original, entry, entryArgs);
  emitReturnInstForDifferential();
  // If errors occurred, back out.
  if (errorOccurred)
    return true;
  LLVM_DEBUG(getADDebugStream()
             << "Generated JVP for " << original->getName() << ":\n"
             << *jvp);
  LLVM_DEBUG(getADDebugStream()
             << "Generated differential for " << original->getName() << ":\n"
             << getDifferential());
  return errorOccurred;
}

bool JVPCloner::run() { return impl.run(); }

} // end namespace autodiff
} // end namespace swift
