//===--- SemaDeclAttr.cpp - Declaration Attribute Handling ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements decl-related attribute processing.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTMutationListener.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/DynamicRecursiveASTVisitor.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/Type.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Cuda.h"
#include "clang/Basic/DarwinSDKInfo.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Attr.h"
#include "clang/Sema/DeclSpec.h"
#include "clang/Sema/DelayedDiagnostic.h"
#include "clang/Sema/Initialization.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/ParsedAttr.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/ScopeInfo.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaAMDGPU.h"
#include "clang/Sema/SemaARM.h"
#include "clang/Sema/SemaAVR.h"
#include "clang/Sema/SemaBPF.h"
#include "clang/Sema/SemaCUDA.h"
#include "clang/Sema/SemaHLSL.h"
#include "clang/Sema/SemaM68k.h"
#include "clang/Sema/SemaMIPS.h"
#include "clang/Sema/SemaMSP430.h"
#include "clang/Sema/SemaObjC.h"
#include "clang/Sema/SemaOpenCL.h"
#include "clang/Sema/SemaOpenMP.h"
#include "clang/Sema/SemaRISCV.h"
#include "clang/Sema/SemaSYCL.h"
#include "clang/Sema/SemaSwift.h"
#include "clang/Sema/SemaWasm.h"
#include "clang/Sema/SemaX86.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/MC/MCSectionMachO.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include <optional>

using namespace clang;
using namespace sema;

namespace AttributeLangSupport {
  enum LANG {
    C,
    Cpp,
    ObjC
  };
} // end namespace AttributeLangSupport

static unsigned getNumAttributeArgs(const ParsedAttr &AL) {
  // FIXME: Include the type in the argument list.
  return AL.getNumArgs() + AL.hasParsedType();
}

SourceLocation Sema::getAttrLoc(const AttributeCommonInfo &CI) {
  return CI.getLoc();
}

/// Wrapper around checkUInt32Argument, with an extra check to be sure
/// that the result will fit into a regular (signed) int. All args have the same
/// purpose as they do in checkUInt32Argument.
template <typename AttrInfo>
static bool checkPositiveIntArgument(Sema &S, const AttrInfo &AI, const Expr *Expr,
                                     int &Val, unsigned Idx = UINT_MAX) {
  uint32_t UVal;
  if (!S.checkUInt32Argument(AI, Expr, UVal, Idx))
    return false;

  if (UVal > (uint32_t)std::numeric_limits<int>::max()) {
    llvm::APSInt I(32); // for toString
    I = UVal;
    S.Diag(Expr->getExprLoc(), diag::err_ice_too_large)
        << toString(I, 10, false) << 32 << /* Unsigned */ 0;
    return false;
  }

  Val = UVal;
  return true;
}

bool Sema::checkStringLiteralArgumentAttr(const AttributeCommonInfo &CI,
                                          const Expr *E, StringRef &Str,
                                          SourceLocation *ArgLocation) {
  const auto *Literal = dyn_cast<StringLiteral>(E->IgnoreParenCasts());
  if (ArgLocation)
    *ArgLocation = E->getBeginLoc();

  if (!Literal || (!Literal->isUnevaluated() && !Literal->isOrdinary())) {
    Diag(E->getBeginLoc(), diag::err_attribute_argument_type)
        << CI << AANT_ArgumentString;
    return false;
  }

  Str = Literal->getString();
  return true;
}

bool Sema::checkStringLiteralArgumentAttr(const ParsedAttr &AL, unsigned ArgNum,
                                          StringRef &Str,
                                          SourceLocation *ArgLocation) {
  // Look for identifiers. If we have one emit a hint to fix it to a literal.
  if (AL.isArgIdent(ArgNum)) {
    IdentifierLoc *Loc = AL.getArgAsIdent(ArgNum);
    Diag(Loc->getLoc(), diag::err_attribute_argument_type)
        << AL << AANT_ArgumentString
        << FixItHint::CreateInsertion(Loc->getLoc(), "\"")
        << FixItHint::CreateInsertion(getLocForEndOfToken(Loc->getLoc()), "\"");
    Str = Loc->getIdentifierInfo()->getName();
    if (ArgLocation)
      *ArgLocation = Loc->getLoc();
    return true;
  }

  // Now check for an actual string literal.
  Expr *ArgExpr = AL.getArgAsExpr(ArgNum);
  const auto *Literal = dyn_cast<StringLiteral>(ArgExpr->IgnoreParenCasts());
  if (ArgLocation)
    *ArgLocation = ArgExpr->getBeginLoc();

  if (!Literal || (!Literal->isUnevaluated() && !Literal->isOrdinary())) {
    Diag(ArgExpr->getBeginLoc(), diag::err_attribute_argument_type)
        << AL << AANT_ArgumentString;
    return false;
  }
  Str = Literal->getString();
  return checkStringLiteralArgumentAttr(AL, ArgExpr, Str, ArgLocation);
}

/// Check if the passed-in expression is of type int or bool.
static bool isIntOrBool(Expr *Exp) {
  QualType QT = Exp->getType();
  return QT->isBooleanType() || QT->isIntegerType();
}


// Check to see if the type is a smart pointer of some kind.  We assume
// it's a smart pointer if it defines both operator-> and operator*.
static bool threadSafetyCheckIsSmartPointer(Sema &S, const RecordType* RT) {
  auto IsOverloadedOperatorPresent = [&S](const RecordDecl *Record,
                                          OverloadedOperatorKind Op) {
    DeclContextLookupResult Result =
        Record->lookup(S.Context.DeclarationNames.getCXXOperatorName(Op));
    return !Result.empty();
  };

  const RecordDecl *Record = RT->getDecl();
  bool foundStarOperator = IsOverloadedOperatorPresent(Record, OO_Star);
  bool foundArrowOperator = IsOverloadedOperatorPresent(Record, OO_Arrow);
  if (foundStarOperator && foundArrowOperator)
    return true;

  const CXXRecordDecl *CXXRecord = dyn_cast<CXXRecordDecl>(Record);
  if (!CXXRecord)
    return false;

  for (const auto &BaseSpecifier : CXXRecord->bases()) {
    if (!foundStarOperator)
      foundStarOperator = IsOverloadedOperatorPresent(
          BaseSpecifier.getType()->getAsRecordDecl(), OO_Star);
    if (!foundArrowOperator)
      foundArrowOperator = IsOverloadedOperatorPresent(
          BaseSpecifier.getType()->getAsRecordDecl(), OO_Arrow);
  }

  if (foundStarOperator && foundArrowOperator)
    return true;

  return false;
}

/// Check if passed in Decl is a pointer type.
/// Note that this function may produce an error message.
/// \return true if the Decl is a pointer type; false otherwise
static bool threadSafetyCheckIsPointer(Sema &S, const Decl *D,
                                       const ParsedAttr &AL) {
  const auto *VD = cast<ValueDecl>(D);
  QualType QT = VD->getType();
  if (QT->isAnyPointerType())
    return true;

  if (const auto *RT = QT->getAs<RecordType>()) {
    // If it's an incomplete type, it could be a smart pointer; skip it.
    // (We don't want to force template instantiation if we can avoid it,
    // since that would alter the order in which templates are instantiated.)
    if (RT->isIncompleteType())
      return true;

    if (threadSafetyCheckIsSmartPointer(S, RT))
      return true;
  }

  S.Diag(AL.getLoc(), diag::warn_thread_attribute_decl_not_pointer) << AL << QT;
  return false;
}

/// Checks that the passed in QualType either is of RecordType or points
/// to RecordType. Returns the relevant RecordType, null if it does not exit.
static const RecordType *getRecordType(QualType QT) {
  if (const auto *RT = QT->getAs<RecordType>())
    return RT;

  // Now check if we point to record type.
  if (const auto *PT = QT->getAs<PointerType>())
    return PT->getPointeeType()->getAs<RecordType>();

  return nullptr;
}

template <typename AttrType>
static bool checkRecordDeclForAttr(const RecordDecl *RD) {
  // Check if the record itself has the attribute.
  if (RD->hasAttr<AttrType>())
    return true;

  // Else check if any base classes have the attribute.
  if (const auto *CRD = dyn_cast<CXXRecordDecl>(RD)) {
    if (!CRD->forallBases([](const CXXRecordDecl *Base) {
          return !Base->hasAttr<AttrType>();
        }))
      return true;
  }
  return false;
}

static bool checkRecordTypeForCapability(Sema &S, QualType Ty) {
  const RecordType *RT = getRecordType(Ty);

  if (!RT)
    return false;

  // Don't check for the capability if the class hasn't been defined yet.
  if (RT->isIncompleteType())
    return true;

  // Allow smart pointers to be used as capability objects.
  // FIXME -- Check the type that the smart pointer points to.
  if (threadSafetyCheckIsSmartPointer(S, RT))
    return true;

  return checkRecordDeclForAttr<CapabilityAttr>(RT->getDecl());
}

static bool checkRecordTypeForScopedCapability(Sema &S, QualType Ty) {
  const RecordType *RT = getRecordType(Ty);

  if (!RT)
    return false;

  // Don't check for the capability if the class hasn't been defined yet.
  if (RT->isIncompleteType())
    return true;

  return checkRecordDeclForAttr<ScopedLockableAttr>(RT->getDecl());
}

static bool checkTypedefTypeForCapability(QualType Ty) {
  const auto *TD = Ty->getAs<TypedefType>();
  if (!TD)
    return false;

  TypedefNameDecl *TN = TD->getDecl();
  if (!TN)
    return false;

  return TN->hasAttr<CapabilityAttr>();
}

static bool typeHasCapability(Sema &S, QualType Ty) {
  if (checkTypedefTypeForCapability(Ty))
    return true;

  if (checkRecordTypeForCapability(S, Ty))
    return true;

  return false;
}

static bool isCapabilityExpr(Sema &S, const Expr *Ex) {
  // Capability expressions are simple expressions involving the boolean logic
  // operators &&, || or !, a simple DeclRefExpr, CastExpr or a ParenExpr. Once
  // a DeclRefExpr is found, its type should be checked to determine whether it
  // is a capability or not.

  if (const auto *E = dyn_cast<CastExpr>(Ex))
    return isCapabilityExpr(S, E->getSubExpr());
  else if (const auto *E = dyn_cast<ParenExpr>(Ex))
    return isCapabilityExpr(S, E->getSubExpr());
  else if (const auto *E = dyn_cast<UnaryOperator>(Ex)) {
    if (E->getOpcode() == UO_LNot || E->getOpcode() == UO_AddrOf ||
        E->getOpcode() == UO_Deref)
      return isCapabilityExpr(S, E->getSubExpr());
    return false;
  } else if (const auto *E = dyn_cast<BinaryOperator>(Ex)) {
    if (E->getOpcode() == BO_LAnd || E->getOpcode() == BO_LOr)
      return isCapabilityExpr(S, E->getLHS()) &&
             isCapabilityExpr(S, E->getRHS());
    return false;
  }

  return typeHasCapability(S, Ex->getType());
}

/// Checks that all attribute arguments, starting from Sidx, resolve to
/// a capability object.
/// \param Sidx The attribute argument index to start checking with.
/// \param ParamIdxOk Whether an argument can be indexing into a function
/// parameter list.
static void checkAttrArgsAreCapabilityObjs(Sema &S, Decl *D,
                                           const ParsedAttr &AL,
                                           SmallVectorImpl<Expr *> &Args,
                                           unsigned Sidx = 0,
                                           bool ParamIdxOk = false) {
  if (Sidx == AL.getNumArgs()) {
    // If we don't have any capability arguments, the attribute implicitly
    // refers to 'this'. So we need to make sure that 'this' exists, i.e. we're
    // a non-static method, and that the class is a (scoped) capability.
    const auto *MD = dyn_cast<const CXXMethodDecl>(D);
    if (MD && !MD->isStatic()) {
      const CXXRecordDecl *RD = MD->getParent();
      // FIXME -- need to check this again on template instantiation
      if (!checkRecordDeclForAttr<CapabilityAttr>(RD) &&
          !checkRecordDeclForAttr<ScopedLockableAttr>(RD))
        S.Diag(AL.getLoc(),
               diag::warn_thread_attribute_not_on_capability_member)
            << AL << MD->getParent();
    } else {
      S.Diag(AL.getLoc(), diag::warn_thread_attribute_not_on_non_static_member)
          << AL;
    }
  }

  for (unsigned Idx = Sidx; Idx < AL.getNumArgs(); ++Idx) {
    Expr *ArgExp = AL.getArgAsExpr(Idx);

    if (ArgExp->isTypeDependent()) {
      // FIXME -- need to check this again on template instantiation
      Args.push_back(ArgExp);
      continue;
    }

    if (const auto *StrLit = dyn_cast<StringLiteral>(ArgExp)) {
      if (StrLit->getLength() == 0 ||
          (StrLit->isOrdinary() && StrLit->getString() == "*")) {
        // Pass empty strings to the analyzer without warnings.
        // Treat "*" as the universal lock.
        Args.push_back(ArgExp);
        continue;
      }

      // We allow constant strings to be used as a placeholder for expressions
      // that are not valid C++ syntax, but warn that they are ignored.
      S.Diag(AL.getLoc(), diag::warn_thread_attribute_ignored) << AL;
      Args.push_back(ArgExp);
      continue;
    }

    QualType ArgTy = ArgExp->getType();

    // A pointer to member expression of the form  &MyClass::mu is treated
    // specially -- we need to look at the type of the member.
    if (const auto *UOp = dyn_cast<UnaryOperator>(ArgExp))
      if (UOp->getOpcode() == UO_AddrOf)
        if (const auto *DRE = dyn_cast<DeclRefExpr>(UOp->getSubExpr()))
          if (DRE->getDecl()->isCXXInstanceMember())
            ArgTy = DRE->getDecl()->getType();

    // First see if we can just cast to record type, or pointer to record type.
    const RecordType *RT = getRecordType(ArgTy);

    // Now check if we index into a record type function param.
    if(!RT && ParamIdxOk) {
      const auto *FD = dyn_cast<FunctionDecl>(D);
      const auto *IL = dyn_cast<IntegerLiteral>(ArgExp);
      if(FD && IL) {
        unsigned int NumParams = FD->getNumParams();
        llvm::APInt ArgValue = IL->getValue();
        uint64_t ParamIdxFromOne = ArgValue.getZExtValue();
        uint64_t ParamIdxFromZero = ParamIdxFromOne - 1;
        if (!ArgValue.isStrictlyPositive() || ParamIdxFromOne > NumParams) {
          S.Diag(AL.getLoc(),
                 diag::err_attribute_argument_out_of_bounds_extra_info)
              << AL << Idx + 1 << NumParams;
          continue;
        }
        ArgTy = FD->getParamDecl(ParamIdxFromZero)->getType();
      }
    }

    // If the type does not have a capability, see if the components of the
    // expression have capabilities. This allows for writing C code where the
    // capability may be on the type, and the expression is a capability
    // boolean logic expression. Eg) requires_capability(A || B && !C)
    if (!typeHasCapability(S, ArgTy) && !isCapabilityExpr(S, ArgExp))
      S.Diag(AL.getLoc(), diag::warn_thread_attribute_argument_not_lockable)
          << AL << ArgTy;

    Args.push_back(ArgExp);
  }
}

static bool checkFunParamsAreScopedLockable(Sema &S,
                                            const ParmVarDecl *ParamDecl,
                                            const ParsedAttr &AL) {
  QualType ParamType = ParamDecl->getType();
  if (const auto *RefType = ParamType->getAs<ReferenceType>();
      RefType &&
      checkRecordTypeForScopedCapability(S, RefType->getPointeeType()))
    return true;
  S.Diag(AL.getLoc(), diag::warn_thread_attribute_not_on_scoped_lockable_param)
      << AL;
  return false;
}

//===----------------------------------------------------------------------===//
// Attribute Implementations
//===----------------------------------------------------------------------===//

static void handlePtGuardedVarAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!threadSafetyCheckIsPointer(S, D, AL))
    return;

  D->addAttr(::new (S.Context) PtGuardedVarAttr(S.Context, AL));
}

static bool checkGuardedByAttrCommon(Sema &S, Decl *D, const ParsedAttr &AL,
                                     Expr *&Arg) {
  SmallVector<Expr *, 1> Args;
  // check that all arguments are lockable objects
  checkAttrArgsAreCapabilityObjs(S, D, AL, Args);
  unsigned Size = Args.size();
  if (Size != 1)
    return false;

  Arg = Args[0];

  return true;
}

static void handleGuardedByAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  Expr *Arg = nullptr;
  if (!checkGuardedByAttrCommon(S, D, AL, Arg))
    return;

  D->addAttr(::new (S.Context) GuardedByAttr(S.Context, AL, Arg));
}

static void handlePtGuardedByAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  Expr *Arg = nullptr;
  if (!checkGuardedByAttrCommon(S, D, AL, Arg))
    return;

  if (!threadSafetyCheckIsPointer(S, D, AL))
    return;

  D->addAttr(::new (S.Context) PtGuardedByAttr(S.Context, AL, Arg));
}

static bool checkAcquireOrderAttrCommon(Sema &S, Decl *D, const ParsedAttr &AL,
                                        SmallVectorImpl<Expr *> &Args) {
  if (!AL.checkAtLeastNumArgs(S, 1))
    return false;

  // Check that this attribute only applies to lockable types.
  QualType QT = cast<ValueDecl>(D)->getType();
  if (!QT->isDependentType() && !typeHasCapability(S, QT)) {
    S.Diag(AL.getLoc(), diag::warn_thread_attribute_decl_not_lockable) << AL;
    return false;
  }

  // Check that all arguments are lockable objects.
  checkAttrArgsAreCapabilityObjs(S, D, AL, Args);
  if (Args.empty())
    return false;

  return true;
}

static void handleAcquiredAfterAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  SmallVector<Expr *, 1> Args;
  if (!checkAcquireOrderAttrCommon(S, D, AL, Args))
    return;

  Expr **StartArg = &Args[0];
  D->addAttr(::new (S.Context)
                 AcquiredAfterAttr(S.Context, AL, StartArg, Args.size()));
}

static void handleAcquiredBeforeAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  SmallVector<Expr *, 1> Args;
  if (!checkAcquireOrderAttrCommon(S, D, AL, Args))
    return;

  Expr **StartArg = &Args[0];
  D->addAttr(::new (S.Context)
                 AcquiredBeforeAttr(S.Context, AL, StartArg, Args.size()));
}

static bool checkLockFunAttrCommon(Sema &S, Decl *D, const ParsedAttr &AL,
                                   SmallVectorImpl<Expr *> &Args) {
  // zero or more arguments ok
  // check that all arguments are lockable objects
  checkAttrArgsAreCapabilityObjs(S, D, AL, Args, 0, /*ParamIdxOk=*/true);

  return true;
}

/// Checks to be sure that the given parameter number is in bounds, and
/// is an integral type. Will emit appropriate diagnostics if this returns
/// false.
///
/// AttrArgNo is used to actually retrieve the argument, so it's base-0.
template <typename AttrInfo>
static bool checkParamIsIntegerType(Sema &S, const Decl *D, const AttrInfo &AI,
                                    unsigned AttrArgNo) {
  assert(AI.isArgExpr(AttrArgNo) && "Expected expression argument");
  Expr *AttrArg = AI.getArgAsExpr(AttrArgNo);
  ParamIdx Idx;
  if (!S.checkFunctionOrMethodParameterIndex(D, AI, AttrArgNo + 1, AttrArg,
                                             Idx))
    return false;

  QualType ParamTy = getFunctionOrMethodParamType(D, Idx.getASTIndex());
  if (!ParamTy->isIntegerType() && !ParamTy->isCharType()) {
    SourceLocation SrcLoc = AttrArg->getBeginLoc();
    S.Diag(SrcLoc, diag::err_attribute_integers_only)
        << AI << getFunctionOrMethodParamRange(D, Idx.getASTIndex());
    return false;
  }
  return true;
}

static void handleAllocSizeAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!AL.checkAtLeastNumArgs(S, 1) || !AL.checkAtMostNumArgs(S, 2))
    return;

  assert(isFuncOrMethodForAttrSubject(D) && hasFunctionProto(D));

  QualType RetTy = getFunctionOrMethodResultType(D);
  if (!RetTy->isPointerType()) {
    S.Diag(AL.getLoc(), diag::warn_attribute_return_pointers_only) << AL;
    return;
  }

  const Expr *SizeExpr = AL.getArgAsExpr(0);
  int SizeArgNoVal;
  // Parameter indices are 1-indexed, hence Index=1
  if (!checkPositiveIntArgument(S, AL, SizeExpr, SizeArgNoVal, /*Idx=*/1))
    return;
  if (!checkParamIsIntegerType(S, D, AL, /*AttrArgNo=*/0))
    return;
  ParamIdx SizeArgNo(SizeArgNoVal, D);

  ParamIdx NumberArgNo;
  if (AL.getNumArgs() == 2) {
    const Expr *NumberExpr = AL.getArgAsExpr(1);
    int Val;
    // Parameter indices are 1-based, hence Index=2
    if (!checkPositiveIntArgument(S, AL, NumberExpr, Val, /*Idx=*/2))
      return;
    if (!checkParamIsIntegerType(S, D, AL, /*AttrArgNo=*/1))
      return;
    NumberArgNo = ParamIdx(Val, D);
  }

  D->addAttr(::new (S.Context)
                 AllocSizeAttr(S.Context, AL, SizeArgNo, NumberArgNo));
}

static bool checkTryLockFunAttrCommon(Sema &S, Decl *D, const ParsedAttr &AL,
                                      SmallVectorImpl<Expr *> &Args) {
  if (!AL.checkAtLeastNumArgs(S, 1))
    return false;

  if (!isIntOrBool(AL.getArgAsExpr(0))) {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_n_type)
        << AL << 1 << AANT_ArgumentIntOrBool;
    return false;
  }

  // check that all arguments are lockable objects
  checkAttrArgsAreCapabilityObjs(S, D, AL, Args, 1);

  return true;
}

static void handleLockReturnedAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // check that the argument is lockable object
  SmallVector<Expr*, 1> Args;
  checkAttrArgsAreCapabilityObjs(S, D, AL, Args);
  unsigned Size = Args.size();
  if (Size == 0)
    return;

  D->addAttr(::new (S.Context) LockReturnedAttr(S.Context, AL, Args[0]));
}

static void handleLocksExcludedAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (const auto *ParmDecl = dyn_cast<ParmVarDecl>(D);
      ParmDecl && !checkFunParamsAreScopedLockable(S, ParmDecl, AL))
    return;

  if (!AL.checkAtLeastNumArgs(S, 1))
    return;

  // check that all arguments are lockable objects
  SmallVector<Expr*, 1> Args;
  checkAttrArgsAreCapabilityObjs(S, D, AL, Args);
  unsigned Size = Args.size();
  if (Size == 0)
    return;
  Expr **StartArg = &Args[0];

  D->addAttr(::new (S.Context)
                 LocksExcludedAttr(S.Context, AL, StartArg, Size));
}

static bool checkFunctionConditionAttr(Sema &S, Decl *D, const ParsedAttr &AL,
                                       Expr *&Cond, StringRef &Msg) {
  Cond = AL.getArgAsExpr(0);
  if (!Cond->isTypeDependent()) {
    ExprResult Converted = S.PerformContextuallyConvertToBool(Cond);
    if (Converted.isInvalid())
      return false;
    Cond = Converted.get();
  }

  if (!S.checkStringLiteralArgumentAttr(AL, 1, Msg))
    return false;

  if (Msg.empty())
    Msg = "<no message provided>";

  SmallVector<PartialDiagnosticAt, 8> Diags;
  if (isa<FunctionDecl>(D) && !Cond->isValueDependent() &&
      !Expr::isPotentialConstantExprUnevaluated(Cond, cast<FunctionDecl>(D),
                                                Diags)) {
    S.Diag(AL.getLoc(), diag::err_attr_cond_never_constant_expr) << AL;
    for (const PartialDiagnosticAt &PDiag : Diags)
      S.Diag(PDiag.first, PDiag.second);
    return false;
  }
  return true;
}

static void handleEnableIfAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  S.Diag(AL.getLoc(), diag::ext_clang_enable_if);

  Expr *Cond;
  StringRef Msg;
  if (checkFunctionConditionAttr(S, D, AL, Cond, Msg))
    D->addAttr(::new (S.Context) EnableIfAttr(S.Context, AL, Cond, Msg));
}

static void handleErrorAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  StringRef NewUserDiagnostic;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, NewUserDiagnostic))
    return;
  if (ErrorAttr *EA = S.mergeErrorAttr(D, AL, NewUserDiagnostic))
    D->addAttr(EA);
}

static void handleExcludeFromExplicitInstantiationAttr(Sema &S, Decl *D,
                                                       const ParsedAttr &AL) {
  const auto *PD = isa<CXXRecordDecl>(D)
                       ? cast<DeclContext>(D)
                       : D->getDeclContext()->getRedeclContext();
  if (const auto *RD = dyn_cast<CXXRecordDecl>(PD); RD && RD->isLocalClass()) {
    S.Diag(AL.getLoc(),
           diag::warn_attribute_exclude_from_explicit_instantiation_local_class)
        << AL << /*IsMember=*/!isa<CXXRecordDecl>(D);
    return;
  }
  D->addAttr(::new (S.Context)
                 ExcludeFromExplicitInstantiationAttr(S.Context, AL));
}

namespace {
/// Determines if a given Expr references any of the given function's
/// ParmVarDecls, or the function's implicit `this` parameter (if applicable).
class ArgumentDependenceChecker : public DynamicRecursiveASTVisitor {
#ifndef NDEBUG
  const CXXRecordDecl *ClassType;
#endif
  llvm::SmallPtrSet<const ParmVarDecl *, 16> Parms;
  bool Result;

public:
  ArgumentDependenceChecker(const FunctionDecl *FD) {
#ifndef NDEBUG
    if (const auto *MD = dyn_cast<CXXMethodDecl>(FD))
      ClassType = MD->getParent();
    else
      ClassType = nullptr;
#endif
    Parms.insert(FD->param_begin(), FD->param_end());
  }

  bool referencesArgs(Expr *E) {
    Result = false;
    TraverseStmt(E);
    return Result;
  }

  bool VisitCXXThisExpr(CXXThisExpr *E) override {
    assert(E->getType()->getPointeeCXXRecordDecl() == ClassType &&
           "`this` doesn't refer to the enclosing class?");
    Result = true;
    return false;
  }

  bool VisitDeclRefExpr(DeclRefExpr *DRE) override {
    if (const auto *PVD = dyn_cast<ParmVarDecl>(DRE->getDecl()))
      if (Parms.count(PVD)) {
        Result = true;
        return false;
      }
    return true;
  }
};
}

static void handleDiagnoseAsBuiltinAttr(Sema &S, Decl *D,
                                        const ParsedAttr &AL) {
  const auto *DeclFD = cast<FunctionDecl>(D);

  if (const auto *MethodDecl = dyn_cast<CXXMethodDecl>(DeclFD))
    if (!MethodDecl->isStatic()) {
      S.Diag(AL.getLoc(), diag::err_attribute_no_member_function) << AL;
      return;
    }

  auto DiagnoseType = [&](unsigned Index, AttributeArgumentNType T) {
    SourceLocation Loc = [&]() {
      auto Union = AL.getArg(Index - 1);
      if (auto *E = dyn_cast<Expr *>(Union))
        return E->getBeginLoc();
      return cast<IdentifierLoc *>(Union)->getLoc();
    }();

    S.Diag(Loc, diag::err_attribute_argument_n_type) << AL << Index << T;
  };

  FunctionDecl *AttrFD = [&]() -> FunctionDecl * {
    if (!AL.isArgExpr(0))
      return nullptr;
    auto *F = dyn_cast_if_present<DeclRefExpr>(AL.getArgAsExpr(0));
    if (!F)
      return nullptr;
    return dyn_cast_if_present<FunctionDecl>(F->getFoundDecl());
  }();

  if (!AttrFD || !AttrFD->getBuiltinID(true)) {
    DiagnoseType(1, AANT_ArgumentBuiltinFunction);
    return;
  }

  if (AttrFD->getNumParams() != AL.getNumArgs() - 1) {
    S.Diag(AL.getLoc(), diag::err_attribute_wrong_number_arguments_for)
        << AL << AttrFD << AttrFD->getNumParams();
    return;
  }

  SmallVector<unsigned, 8> Indices;

  for (unsigned I = 1; I < AL.getNumArgs(); ++I) {
    if (!AL.isArgExpr(I)) {
      DiagnoseType(I + 1, AANT_ArgumentIntegerConstant);
      return;
    }

    const Expr *IndexExpr = AL.getArgAsExpr(I);
    uint32_t Index;

    if (!S.checkUInt32Argument(AL, IndexExpr, Index, I + 1, false))
      return;

    if (Index > DeclFD->getNumParams()) {
      S.Diag(AL.getLoc(), diag::err_attribute_bounds_for_function)
          << AL << Index << DeclFD << DeclFD->getNumParams();
      return;
    }

    QualType T1 = AttrFD->getParamDecl(I - 1)->getType();
    QualType T2 = DeclFD->getParamDecl(Index - 1)->getType();

    if (T1.getCanonicalType().getUnqualifiedType() !=
        T2.getCanonicalType().getUnqualifiedType()) {
      S.Diag(IndexExpr->getBeginLoc(), diag::err_attribute_parameter_types)
          << AL << Index << DeclFD << T2 << I << AttrFD << T1;
      return;
    }

    Indices.push_back(Index - 1);
  }

  D->addAttr(::new (S.Context) DiagnoseAsBuiltinAttr(
      S.Context, AL, AttrFD, Indices.data(), Indices.size()));
}

static void handleDiagnoseIfAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  S.Diag(AL.getLoc(), diag::ext_clang_diagnose_if);

  Expr *Cond;
  StringRef Msg;
  if (!checkFunctionConditionAttr(S, D, AL, Cond, Msg))
    return;

  StringRef DefaultSevStr;
  if (!S.checkStringLiteralArgumentAttr(AL, 2, DefaultSevStr))
    return;

  DiagnoseIfAttr::DefaultSeverity DefaultSev;
  if (!DiagnoseIfAttr::ConvertStrToDefaultSeverity(DefaultSevStr, DefaultSev)) {
    S.Diag(AL.getArgAsExpr(2)->getBeginLoc(),
           diag::err_diagnose_if_invalid_diagnostic_type);
    return;
  }

  StringRef WarningGroup;
  if (AL.getNumArgs() > 3) {
    if (!S.checkStringLiteralArgumentAttr(AL, 3, WarningGroup))
      return;
    if (WarningGroup.empty() ||
        !S.getDiagnostics().getDiagnosticIDs()->getGroupForWarningOption(
            WarningGroup)) {
      S.Diag(AL.getArgAsExpr(3)->getBeginLoc(),
             diag::err_diagnose_if_unknown_warning)
          << WarningGroup;
      return;
    }
  }

  bool ArgDependent = false;
  if (const auto *FD = dyn_cast<FunctionDecl>(D))
    ArgDependent = ArgumentDependenceChecker(FD).referencesArgs(Cond);
  D->addAttr(::new (S.Context) DiagnoseIfAttr(
      S.Context, AL, Cond, Msg, DefaultSev, WarningGroup, ArgDependent,
      cast<NamedDecl>(D)));
}

static void handleCFIUncheckedCalleeAttr(Sema &S, Decl *D,
                                         const ParsedAttr &Attrs) {
  if (hasDeclarator(D))
    return;

  if (!isa<ObjCMethodDecl>(D)) {
    S.Diag(Attrs.getLoc(), diag::warn_attribute_wrong_decl_type)
        << Attrs << Attrs.isRegularKeywordAttribute()
        << ExpectedFunctionOrMethod;
    return;
  }

  D->addAttr(::new (S.Context) CFIUncheckedCalleeAttr(S.Context, Attrs));
}

static void handleNoBuiltinAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  static constexpr const StringRef kWildcard = "*";

  llvm::SmallVector<StringRef, 16> Names;
  bool HasWildcard = false;

  const auto AddBuiltinName = [&Names, &HasWildcard](StringRef Name) {
    if (Name == kWildcard)
      HasWildcard = true;
    Names.push_back(Name);
  };

  // Add previously defined attributes.
  if (const auto *NBA = D->getAttr<NoBuiltinAttr>())
    for (StringRef BuiltinName : NBA->builtinNames())
      AddBuiltinName(BuiltinName);

  // Add current attributes.
  if (AL.getNumArgs() == 0)
    AddBuiltinName(kWildcard);
  else
    for (unsigned I = 0, E = AL.getNumArgs(); I != E; ++I) {
      StringRef BuiltinName;
      SourceLocation LiteralLoc;
      if (!S.checkStringLiteralArgumentAttr(AL, I, BuiltinName, &LiteralLoc))
        return;

      if (Builtin::Context::isBuiltinFunc(BuiltinName))
        AddBuiltinName(BuiltinName);
      else
        S.Diag(LiteralLoc, diag::warn_attribute_no_builtin_invalid_builtin_name)
            << BuiltinName << AL;
    }

  // Repeating the same attribute is fine.
  llvm::sort(Names);
  Names.erase(llvm::unique(Names), Names.end());

  // Empty no_builtin must be on its own.
  if (HasWildcard && Names.size() > 1)
    S.Diag(D->getLocation(),
           diag::err_attribute_no_builtin_wildcard_or_builtin_name)
        << AL;

  if (D->hasAttr<NoBuiltinAttr>())
    D->dropAttr<NoBuiltinAttr>();
  D->addAttr(::new (S.Context)
                 NoBuiltinAttr(S.Context, AL, Names.data(), Names.size()));
}

static void handlePassObjectSizeAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (D->hasAttr<PassObjectSizeAttr>()) {
    S.Diag(D->getBeginLoc(), diag::err_attribute_only_once_per_parameter) << AL;
    return;
  }

  Expr *E = AL.getArgAsExpr(0);
  uint32_t Type;
  if (!S.checkUInt32Argument(AL, E, Type, /*Idx=*/1))
    return;

  // pass_object_size's argument is passed in as the second argument of
  // __builtin_object_size. So, it has the same constraints as that second
  // argument; namely, it must be in the range [0, 3].
  if (Type > 3) {
    S.Diag(E->getBeginLoc(), diag::err_attribute_argument_out_of_range)
        << AL << 0 << 3 << E->getSourceRange();
    return;
  }

  // pass_object_size is only supported on constant pointer parameters; as a
  // kindness to users, we allow the parameter to be non-const for declarations.
  // At this point, we have no clue if `D` belongs to a function declaration or
  // definition, so we defer the constness check until later.
  if (!cast<ParmVarDecl>(D)->getType()->isPointerType()) {
    S.Diag(D->getBeginLoc(), diag::err_attribute_pointers_only) << AL << 1;
    return;
  }

  D->addAttr(::new (S.Context) PassObjectSizeAttr(S.Context, AL, (int)Type));
}

static void handleConsumableAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  ConsumableAttr::ConsumedState DefaultState;

  if (AL.isArgIdent(0)) {
    IdentifierLoc *IL = AL.getArgAsIdent(0);
    if (!ConsumableAttr::ConvertStrToConsumedState(
            IL->getIdentifierInfo()->getName(), DefaultState)) {
      S.Diag(IL->getLoc(), diag::warn_attribute_type_not_supported)
          << AL << IL->getIdentifierInfo();
      return;
    }
  } else {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_type)
        << AL << AANT_ArgumentIdentifier;
    return;
  }

  D->addAttr(::new (S.Context) ConsumableAttr(S.Context, AL, DefaultState));
}

static bool checkForConsumableClass(Sema &S, const CXXMethodDecl *MD,
                                    const ParsedAttr &AL) {
  QualType ThisType = MD->getFunctionObjectParameterType();

  if (const CXXRecordDecl *RD = ThisType->getAsCXXRecordDecl()) {
    if (!RD->hasAttr<ConsumableAttr>()) {
      S.Diag(AL.getLoc(), diag::warn_attr_on_unconsumable_class) << RD;

      return false;
    }
  }

  return true;
}

static void handleCallableWhenAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!AL.checkAtLeastNumArgs(S, 1))
    return;

  if (!checkForConsumableClass(S, cast<CXXMethodDecl>(D), AL))
    return;

  SmallVector<CallableWhenAttr::ConsumedState, 3> States;
  for (unsigned ArgIndex = 0; ArgIndex < AL.getNumArgs(); ++ArgIndex) {
    CallableWhenAttr::ConsumedState CallableState;

    StringRef StateString;
    SourceLocation Loc;
    if (AL.isArgIdent(ArgIndex)) {
      IdentifierLoc *Ident = AL.getArgAsIdent(ArgIndex);
      StateString = Ident->getIdentifierInfo()->getName();
      Loc = Ident->getLoc();
    } else {
      if (!S.checkStringLiteralArgumentAttr(AL, ArgIndex, StateString, &Loc))
        return;
    }

    if (!CallableWhenAttr::ConvertStrToConsumedState(StateString,
                                                     CallableState)) {
      S.Diag(Loc, diag::warn_attribute_type_not_supported) << AL << StateString;
      return;
    }

    States.push_back(CallableState);
  }

  D->addAttr(::new (S.Context)
                 CallableWhenAttr(S.Context, AL, States.data(), States.size()));
}

static void handleParamTypestateAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  ParamTypestateAttr::ConsumedState ParamState;

  if (AL.isArgIdent(0)) {
    IdentifierLoc *Ident = AL.getArgAsIdent(0);
    StringRef StateString = Ident->getIdentifierInfo()->getName();

    if (!ParamTypestateAttr::ConvertStrToConsumedState(StateString,
                                                       ParamState)) {
      S.Diag(Ident->getLoc(), diag::warn_attribute_type_not_supported)
          << AL << StateString;
      return;
    }
  } else {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_type)
        << AL << AANT_ArgumentIdentifier;
    return;
  }

  // FIXME: This check is currently being done in the analysis.  It can be
  //        enabled here only after the parser propagates attributes at
  //        template specialization definition, not declaration.
  //QualType ReturnType = cast<ParmVarDecl>(D)->getType();
  //const CXXRecordDecl *RD = ReturnType->getAsCXXRecordDecl();
  //
  //if (!RD || !RD->hasAttr<ConsumableAttr>()) {
  //    S.Diag(AL.getLoc(), diag::warn_return_state_for_unconsumable_type) <<
  //      ReturnType.getAsString();
  //    return;
  //}

  D->addAttr(::new (S.Context) ParamTypestateAttr(S.Context, AL, ParamState));
}

static void handleReturnTypestateAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  ReturnTypestateAttr::ConsumedState ReturnState;

  if (AL.isArgIdent(0)) {
    IdentifierLoc *IL = AL.getArgAsIdent(0);
    if (!ReturnTypestateAttr::ConvertStrToConsumedState(
            IL->getIdentifierInfo()->getName(), ReturnState)) {
      S.Diag(IL->getLoc(), diag::warn_attribute_type_not_supported)
          << AL << IL->getIdentifierInfo();
      return;
    }
  } else {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_type)
        << AL << AANT_ArgumentIdentifier;
    return;
  }

  // FIXME: This check is currently being done in the analysis.  It can be
  //        enabled here only after the parser propagates attributes at
  //        template specialization definition, not declaration.
  // QualType ReturnType;
  //
  // if (const ParmVarDecl *Param = dyn_cast<ParmVarDecl>(D)) {
  //  ReturnType = Param->getType();
  //
  //} else if (const CXXConstructorDecl *Constructor =
  //             dyn_cast<CXXConstructorDecl>(D)) {
  //  ReturnType = Constructor->getFunctionObjectParameterType();
  //
  //} else {
  //
  //  ReturnType = cast<FunctionDecl>(D)->getCallResultType();
  //}
  //
  // const CXXRecordDecl *RD = ReturnType->getAsCXXRecordDecl();
  //
  // if (!RD || !RD->hasAttr<ConsumableAttr>()) {
  //    S.Diag(Attr.getLoc(), diag::warn_return_state_for_unconsumable_type) <<
  //      ReturnType.getAsString();
  //    return;
  //}

  D->addAttr(::new (S.Context) ReturnTypestateAttr(S.Context, AL, ReturnState));
}

static void handleSetTypestateAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!checkForConsumableClass(S, cast<CXXMethodDecl>(D), AL))
    return;

  SetTypestateAttr::ConsumedState NewState;
  if (AL.isArgIdent(0)) {
    IdentifierLoc *Ident = AL.getArgAsIdent(0);
    StringRef Param = Ident->getIdentifierInfo()->getName();
    if (!SetTypestateAttr::ConvertStrToConsumedState(Param, NewState)) {
      S.Diag(Ident->getLoc(), diag::warn_attribute_type_not_supported)
          << AL << Param;
      return;
    }
  } else {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_type)
        << AL << AANT_ArgumentIdentifier;
    return;
  }

  D->addAttr(::new (S.Context) SetTypestateAttr(S.Context, AL, NewState));
}

static void handleTestTypestateAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!checkForConsumableClass(S, cast<CXXMethodDecl>(D), AL))
    return;

  TestTypestateAttr::ConsumedState TestState;
  if (AL.isArgIdent(0)) {
    IdentifierLoc *Ident = AL.getArgAsIdent(0);
    StringRef Param = Ident->getIdentifierInfo()->getName();
    if (!TestTypestateAttr::ConvertStrToConsumedState(Param, TestState)) {
      S.Diag(Ident->getLoc(), diag::warn_attribute_type_not_supported)
          << AL << Param;
      return;
    }
  } else {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_type)
        << AL << AANT_ArgumentIdentifier;
    return;
  }

  D->addAttr(::new (S.Context) TestTypestateAttr(S.Context, AL, TestState));
}

static void handleExtVectorTypeAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // Remember this typedef decl, we will need it later for diagnostics.
  if (isa<TypedefNameDecl>(D))
    S.ExtVectorDecls.push_back(cast<TypedefNameDecl>(D));
}

static void handlePackedAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (auto *TD = dyn_cast<TagDecl>(D))
    TD->addAttr(::new (S.Context) PackedAttr(S.Context, AL));
  else if (auto *FD = dyn_cast<FieldDecl>(D)) {
    bool BitfieldByteAligned = (!FD->getType()->isDependentType() &&
                                !FD->getType()->isIncompleteType() &&
                                FD->isBitField() &&
                                S.Context.getTypeAlign(FD->getType()) <= 8);

    if (S.getASTContext().getTargetInfo().getTriple().isPS()) {
      if (BitfieldByteAligned)
        // The PS4/PS5 targets need to maintain ABI backwards compatibility.
        S.Diag(AL.getLoc(), diag::warn_attribute_ignored_for_field_of_type)
            << AL << FD->getType();
      else
        FD->addAttr(::new (S.Context) PackedAttr(S.Context, AL));
    } else {
      // Report warning about changed offset in the newer compiler versions.
      if (BitfieldByteAligned)
        S.Diag(AL.getLoc(), diag::warn_attribute_packed_for_bitfield);

      FD->addAttr(::new (S.Context) PackedAttr(S.Context, AL));
    }

  } else
    S.Diag(AL.getLoc(), diag::warn_attribute_ignored) << AL;
}

static void handlePreferredName(Sema &S, Decl *D, const ParsedAttr &AL) {
  auto *RD = cast<CXXRecordDecl>(D);
  ClassTemplateDecl *CTD = RD->getDescribedClassTemplate();
  assert(CTD && "attribute does not appertain to this declaration");

  ParsedType PT = AL.getTypeArg();
  TypeSourceInfo *TSI = nullptr;
  QualType T = S.GetTypeFromParser(PT, &TSI);
  if (!TSI)
    TSI = S.Context.getTrivialTypeSourceInfo(T, AL.getLoc());

  if (!T.hasQualifiers() && T->isTypedefNameType()) {
    // Find the template name, if this type names a template specialization.
    const TemplateDecl *Template = nullptr;
    if (const auto *CTSD = dyn_cast_if_present<ClassTemplateSpecializationDecl>(
            T->getAsCXXRecordDecl())) {
      Template = CTSD->getSpecializedTemplate();
    } else if (const auto *TST = T->getAs<TemplateSpecializationType>()) {
      while (TST && TST->isTypeAlias())
        TST = TST->getAliasedType()->getAs<TemplateSpecializationType>();
      if (TST)
        Template = TST->getTemplateName().getAsTemplateDecl();
    }

    if (Template && declaresSameEntity(Template, CTD)) {
      D->addAttr(::new (S.Context) PreferredNameAttr(S.Context, AL, TSI));
      return;
    }
  }

  S.Diag(AL.getLoc(), diag::err_attribute_not_typedef_for_specialization)
      << T << AL << CTD;
  if (const auto *TT = T->getAs<TypedefType>())
    S.Diag(TT->getDecl()->getLocation(), diag::note_entity_declared_at)
        << TT->getDecl();
}

static void handleNoSpecializations(Sema &S, Decl *D, const ParsedAttr &AL) {
  StringRef Message;
  if (AL.getNumArgs() != 0)
    S.checkStringLiteralArgumentAttr(AL, 0, Message);
  D->getDescribedTemplate()->addAttr(
      NoSpecializationsAttr::Create(S.Context, Message, AL));
}

bool Sema::isValidPointerAttrType(QualType T, bool RefOkay) {
  if (T->isDependentType())
    return true;
  if (RefOkay) {
    if (T->isReferenceType())
      return true;
  } else {
    T = T.getNonReferenceType();
  }

  // The nonnull attribute, and other similar attributes, can be applied to a
  // transparent union that contains a pointer type.
  if (const RecordType *UT = T->getAsUnionType()) {
    if (UT && UT->getDecl()->hasAttr<TransparentUnionAttr>()) {
      RecordDecl *UD = UT->getDecl();
      for (const auto *I : UD->fields()) {
        QualType QT = I->getType();
        if (QT->isAnyPointerType() || QT->isBlockPointerType())
          return true;
      }
    }
  }

  return T->isAnyPointerType() || T->isBlockPointerType();
}

static bool attrNonNullArgCheck(Sema &S, QualType T, const ParsedAttr &AL,
                                SourceRange AttrParmRange,
                                SourceRange TypeRange,
                                bool isReturnValue = false) {
  if (!S.isValidPointerAttrType(T)) {
    if (isReturnValue)
      S.Diag(AL.getLoc(), diag::warn_attribute_return_pointers_only)
          << AL << AttrParmRange << TypeRange;
    else
      S.Diag(AL.getLoc(), diag::warn_attribute_pointers_only)
          << AL << AttrParmRange << TypeRange << 0;
    return false;
  }
  return true;
}

static void handleNonNullAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  SmallVector<ParamIdx, 8> NonNullArgs;
  for (unsigned I = 0; I < AL.getNumArgs(); ++I) {
    Expr *Ex = AL.getArgAsExpr(I);
    ParamIdx Idx;
    if (!S.checkFunctionOrMethodParameterIndex(
            D, AL, I + 1, Ex, Idx,
            /*CanIndexImplicitThis=*/false,
            /*CanIndexVariadicArguments=*/true))
      return;

    // Is the function argument a pointer type?
    if (Idx.getASTIndex() < getFunctionOrMethodNumParams(D) &&
        !attrNonNullArgCheck(
            S, getFunctionOrMethodParamType(D, Idx.getASTIndex()), AL,
            Ex->getSourceRange(),
            getFunctionOrMethodParamRange(D, Idx.getASTIndex())))
      continue;

    NonNullArgs.push_back(Idx);
  }

  // If no arguments were specified to __attribute__((nonnull)) then all pointer
  // arguments have a nonnull attribute; warn if there aren't any. Skip this
  // check if the attribute came from a macro expansion or a template
  // instantiation.
  if (NonNullArgs.empty() && AL.getLoc().isFileID() &&
      !S.inTemplateInstantiation()) {
    bool AnyPointers = isFunctionOrMethodVariadic(D);
    for (unsigned I = 0, E = getFunctionOrMethodNumParams(D);
         I != E && !AnyPointers; ++I) {
      QualType T = getFunctionOrMethodParamType(D, I);
      if (S.isValidPointerAttrType(T))
        AnyPointers = true;
    }

    if (!AnyPointers)
      S.Diag(AL.getLoc(), diag::warn_attribute_nonnull_no_pointers);
  }

  ParamIdx *Start = NonNullArgs.data();
  unsigned Size = NonNullArgs.size();
  llvm::array_pod_sort(Start, Start + Size);
  D->addAttr(::new (S.Context) NonNullAttr(S.Context, AL, Start, Size));
}

static void handleNonNullAttrParameter(Sema &S, ParmVarDecl *D,
                                       const ParsedAttr &AL) {
  if (AL.getNumArgs() > 0) {
    if (D->getFunctionType()) {
      handleNonNullAttr(S, D, AL);
    } else {
      S.Diag(AL.getLoc(), diag::warn_attribute_nonnull_parm_no_args)
        << D->getSourceRange();
    }
    return;
  }

  // Is the argument a pointer type?
  if (!attrNonNullArgCheck(S, D->getType(), AL, SourceRange(),
                           D->getSourceRange()))
    return;

  D->addAttr(::new (S.Context) NonNullAttr(S.Context, AL, nullptr, 0));
}

static void handleReturnsNonNullAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  QualType ResultType = getFunctionOrMethodResultType(D);
  SourceRange SR = getFunctionOrMethodResultSourceRange(D);
  if (!attrNonNullArgCheck(S, ResultType, AL, SourceRange(), SR,
                           /* isReturnValue */ true))
    return;

  D->addAttr(::new (S.Context) ReturnsNonNullAttr(S.Context, AL));
}

static void handleNoEscapeAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (D->isInvalidDecl())
    return;

  // noescape only applies to pointer types.
  QualType T = cast<ParmVarDecl>(D)->getType();
  if (!S.isValidPointerAttrType(T, /* RefOkay */ true)) {
    S.Diag(AL.getLoc(), diag::warn_attribute_pointers_only)
        << AL << AL.getRange() << 0;
    return;
  }

  D->addAttr(::new (S.Context) NoEscapeAttr(S.Context, AL));
}

static void handleAssumeAlignedAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  Expr *E = AL.getArgAsExpr(0),
       *OE = AL.getNumArgs() > 1 ? AL.getArgAsExpr(1) : nullptr;
  S.AddAssumeAlignedAttr(D, AL, E, OE);
}

static void handleAllocAlignAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  S.AddAllocAlignAttr(D, AL, AL.getArgAsExpr(0));
}

void Sema::AddAssumeAlignedAttr(Decl *D, const AttributeCommonInfo &CI, Expr *E,
                                Expr *OE) {
  QualType ResultType = getFunctionOrMethodResultType(D);
  SourceRange SR = getFunctionOrMethodResultSourceRange(D);
  SourceLocation AttrLoc = CI.getLoc();

  if (!isValidPointerAttrType(ResultType, /* RefOkay */ true)) {
    Diag(AttrLoc, diag::warn_attribute_return_pointers_refs_only)
        << CI << CI.getRange() << SR;
    return;
  }

  if (!E->isValueDependent()) {
    std::optional<llvm::APSInt> I = llvm::APSInt(64);
    if (!(I = E->getIntegerConstantExpr(Context))) {
      if (OE)
        Diag(AttrLoc, diag::err_attribute_argument_n_type)
            << CI << 1 << AANT_ArgumentIntegerConstant << E->getSourceRange();
      else
        Diag(AttrLoc, diag::err_attribute_argument_type)
            << CI << AANT_ArgumentIntegerConstant << E->getSourceRange();
      return;
    }

    if (!I->isPowerOf2()) {
      Diag(AttrLoc, diag::err_alignment_not_power_of_two)
        << E->getSourceRange();
      return;
    }

    if (*I > Sema::MaximumAlignment)
      Diag(CI.getLoc(), diag::warn_assume_aligned_too_great)
          << CI.getRange() << Sema::MaximumAlignment;
  }

  if (OE && !OE->isValueDependent() && !OE->isIntegerConstantExpr(Context)) {
    Diag(AttrLoc, diag::err_attribute_argument_n_type)
        << CI << 2 << AANT_ArgumentIntegerConstant << OE->getSourceRange();
    return;
  }

  D->addAttr(::new (Context) AssumeAlignedAttr(Context, CI, E, OE));
}

void Sema::AddAllocAlignAttr(Decl *D, const AttributeCommonInfo &CI,
                             Expr *ParamExpr) {
  QualType ResultType = getFunctionOrMethodResultType(D);
  SourceLocation AttrLoc = CI.getLoc();

  if (!isValidPointerAttrType(ResultType, /* RefOkay */ true)) {
    Diag(AttrLoc, diag::warn_attribute_return_pointers_refs_only)
        << CI << CI.getRange() << getFunctionOrMethodResultSourceRange(D);
    return;
  }

  ParamIdx Idx;
  const auto *FuncDecl = cast<FunctionDecl>(D);
  if (!checkFunctionOrMethodParameterIndex(FuncDecl, CI,
                                           /*AttrArgNum=*/1, ParamExpr, Idx))
    return;

  QualType Ty = getFunctionOrMethodParamType(D, Idx.getASTIndex());
  if (!Ty->isDependentType() && !Ty->isIntegralType(Context) &&
      !Ty->isAlignValT()) {
    Diag(ParamExpr->getBeginLoc(), diag::err_attribute_integers_only)
        << CI << FuncDecl->getParamDecl(Idx.getASTIndex())->getSourceRange();
    return;
  }

  D->addAttr(::new (Context) AllocAlignAttr(Context, CI, Idx));
}

/// Normalize the attribute, __foo__ becomes foo.
/// Returns true if normalization was applied.
static bool normalizeName(StringRef &AttrName) {
  if (AttrName.size() > 4 && AttrName.starts_with("__") &&
      AttrName.ends_with("__")) {
    AttrName = AttrName.drop_front(2).drop_back(2);
    return true;
  }
  return false;
}

static void handleOwnershipAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // This attribute must be applied to a function declaration. The first
  // argument to the attribute must be an identifier, the name of the resource,
  // for example: malloc. The following arguments must be argument indexes, the
  // arguments must be of integer type for Returns, otherwise of pointer type.
  // The difference between Holds and Takes is that a pointer may still be used
  // after being held. free() should be __attribute((ownership_takes)), whereas
  // a list append function may well be __attribute((ownership_holds)).

  if (!AL.isArgIdent(0)) {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_n_type)
        << AL << 1 << AANT_ArgumentIdentifier;
    return;
  }

  // Figure out our Kind.
  OwnershipAttr::OwnershipKind K =
      OwnershipAttr(S.Context, AL, nullptr, nullptr, 0).getOwnKind();

  // Check arguments.
  switch (K) {
  case OwnershipAttr::Takes:
  case OwnershipAttr::Holds:
    if (AL.getNumArgs() < 2) {
      S.Diag(AL.getLoc(), diag::err_attribute_too_few_arguments) << AL << 2;
      return;
    }
    break;
  case OwnershipAttr::Returns:
    if (AL.getNumArgs() > 2) {
      S.Diag(AL.getLoc(), diag::err_attribute_too_many_arguments) << AL << 2;
      return;
    }
    break;
  }

  // Allow only pointers to be return type for functions with ownership_returns
  // attribute. This matches with current OwnershipAttr::Takes semantics
  if (K == OwnershipAttr::Returns &&
      !getFunctionOrMethodResultType(D)->isPointerType()) {
    S.Diag(AL.getLoc(), diag::err_ownership_takes_return_type) << AL;
    return;
  }

  IdentifierInfo *Module = AL.getArgAsIdent(0)->getIdentifierInfo();

  StringRef ModuleName = Module->getName();
  if (normalizeName(ModuleName)) {
    Module = &S.PP.getIdentifierTable().get(ModuleName);
  }

  SmallVector<ParamIdx, 8> OwnershipArgs;
  for (unsigned i = 1; i < AL.getNumArgs(); ++i) {
    Expr *Ex = AL.getArgAsExpr(i);
    ParamIdx Idx;
    if (!S.checkFunctionOrMethodParameterIndex(D, AL, i, Ex, Idx))
      return;

    // Is the function argument a pointer type?
    QualType T = getFunctionOrMethodParamType(D, Idx.getASTIndex());
    int Err = -1;  // No error
    switch (K) {
      case OwnershipAttr::Takes:
      case OwnershipAttr::Holds:
        if (!T->isAnyPointerType() && !T->isBlockPointerType())
          Err = 0;
        break;
      case OwnershipAttr::Returns:
        if (!T->isIntegerType())
          Err = 1;
        break;
    }
    if (-1 != Err) {
      S.Diag(AL.getLoc(), diag::err_ownership_type) << AL << Err
                                                    << Ex->getSourceRange();
      return;
    }

    // Check we don't have a conflict with another ownership attribute.
    for (const auto *I : D->specific_attrs<OwnershipAttr>()) {
      // Cannot have two ownership attributes of different kinds for the same
      // index.
      if (I->getOwnKind() != K && llvm::is_contained(I->args(), Idx)) {
          S.Diag(AL.getLoc(), diag::err_attributes_are_not_compatible)
              << AL << I
              << (AL.isRegularKeywordAttribute() ||
                  I->isRegularKeywordAttribute());
          return;
      } else if (K == OwnershipAttr::Returns &&
                 I->getOwnKind() == OwnershipAttr::Returns) {
        // A returns attribute conflicts with any other returns attribute using
        // a different index.
        if (!llvm::is_contained(I->args(), Idx)) {
          S.Diag(I->getLocation(), diag::err_ownership_returns_index_mismatch)
              << I->args_begin()->getSourceIndex();
          if (I->args_size())
            S.Diag(AL.getLoc(), diag::note_ownership_returns_index_mismatch)
                << Idx.getSourceIndex() << Ex->getSourceRange();
          return;
        }
      } else if (K == OwnershipAttr::Takes &&
                 I->getOwnKind() == OwnershipAttr::Takes) {
        if (I->getModule()->getName() != ModuleName) {
          S.Diag(I->getLocation(), diag::err_ownership_takes_class_mismatch)
              << I->getModule()->getName();
          S.Diag(AL.getLoc(), diag::note_ownership_takes_class_mismatch)
              << ModuleName << Ex->getSourceRange();

          return;
        }
      }
    }
    OwnershipArgs.push_back(Idx);
  }

  ParamIdx *Start = OwnershipArgs.data();
  unsigned Size = OwnershipArgs.size();
  llvm::array_pod_sort(Start, Start + Size);
  D->addAttr(::new (S.Context)
                 OwnershipAttr(S.Context, AL, Module, Start, Size));
}

static void handleWeakRefAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // Check the attribute arguments.
  if (AL.getNumArgs() > 1) {
    S.Diag(AL.getLoc(), diag::err_attribute_wrong_number_arguments) << AL << 1;
    return;
  }

  // gcc rejects
  // class c {
  //   static int a __attribute__((weakref ("v2")));
  //   static int b() __attribute__((weakref ("f3")));
  // };
  // and ignores the attributes of
  // void f(void) {
  //   static int a __attribute__((weakref ("v2")));
  // }
  // we reject them
  const DeclContext *Ctx = D->getDeclContext()->getRedeclContext();
  if (!Ctx->isFileContext()) {
    S.Diag(AL.getLoc(), diag::err_attribute_weakref_not_global_context)
        << cast<NamedDecl>(D);
    return;
  }

  // The GCC manual says
  //
  // At present, a declaration to which `weakref' is attached can only
  // be `static'.
  //
  // It also says
  //
  // Without a TARGET,
  // given as an argument to `weakref' or to `alias', `weakref' is
  // equivalent to `weak'.
  //
  // gcc 4.4.1 will accept
  // int a7 __attribute__((weakref));
  // as
  // int a7 __attribute__((weak));
  // This looks like a bug in gcc. We reject that for now. We should revisit
  // it if this behaviour is actually used.

  // GCC rejects
  // static ((alias ("y"), weakref)).
  // Should we? How to check that weakref is before or after alias?

  // FIXME: it would be good for us to keep the WeakRefAttr as-written instead
  // of transforming it into an AliasAttr.  The WeakRefAttr never uses the
  // StringRef parameter it was given anyway.
  StringRef Str;
  if (AL.getNumArgs() && S.checkStringLiteralArgumentAttr(AL, 0, Str))
    // GCC will accept anything as the argument of weakref. Should we
    // check for an existing decl?
    D->addAttr(::new (S.Context) AliasAttr(S.Context, AL, Str));

  D->addAttr(::new (S.Context) WeakRefAttr(S.Context, AL));
}

// Mark alias/ifunc target as used. Due to name mangling, we look up the
// demangled name ignoring parameters (not supported by microsoftDemangle
// https://github.com/llvm/llvm-project/issues/88825). This should handle the
// majority of use cases while leaving namespace scope names unmarked.
static void markUsedForAliasOrIfunc(Sema &S, Decl *D, const ParsedAttr &AL,
                                    StringRef Str) {
  std::unique_ptr<char, llvm::FreeDeleter> Demangled;
  if (S.getASTContext().getCXXABIKind() != TargetCXXABI::Microsoft)
    Demangled.reset(llvm::itaniumDemangle(Str, /*ParseParams=*/false));
  std::unique_ptr<MangleContext> MC(S.Context.createMangleContext());
  SmallString<256> Name;

  const DeclarationNameInfo Target(
      &S.Context.Idents.get(Demangled ? Demangled.get() : Str), AL.getLoc());
  LookupResult LR(S, Target, Sema::LookupOrdinaryName);
  if (S.LookupName(LR, S.TUScope)) {
    for (NamedDecl *ND : LR) {
      if (!isa<FunctionDecl>(ND) && !isa<VarDecl>(ND))
        continue;
      if (MC->shouldMangleDeclName(ND)) {
        llvm::raw_svector_ostream Out(Name);
        Name.clear();
        MC->mangleName(GlobalDecl(ND), Out);
      } else {
        Name = ND->getIdentifier()->getName();
      }
      if (Name == Str)
        ND->markUsed(S.Context);
    }
  }
}

static void handleIFuncAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  StringRef Str;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, Str))
    return;

  // Aliases should be on declarations, not definitions.
  const auto *FD = cast<FunctionDecl>(D);
  if (FD->isThisDeclarationADefinition()) {
    S.Diag(AL.getLoc(), diag::err_alias_is_definition) << FD << 1;
    return;
  }

  markUsedForAliasOrIfunc(S, D, AL, Str);
  D->addAttr(::new (S.Context) IFuncAttr(S.Context, AL, Str));
}

static void handleAliasAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  StringRef Str;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, Str))
    return;

  if (S.Context.getTargetInfo().getTriple().isOSDarwin()) {
    S.Diag(AL.getLoc(), diag::err_alias_not_supported_on_darwin);
    return;
  }

  if (S.Context.getTargetInfo().getTriple().isNVPTX()) {
    CudaVersion Version =
        ToCudaVersion(S.Context.getTargetInfo().getSDKVersion());
    if (Version != CudaVersion::UNKNOWN && Version < CudaVersion::CUDA_100)
      S.Diag(AL.getLoc(), diag::err_alias_not_supported_on_nvptx);
  }

  // Aliases should be on declarations, not definitions.
  if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
    if (FD->isThisDeclarationADefinition()) {
      S.Diag(AL.getLoc(), diag::err_alias_is_definition) << FD << 0;
      return;
    }
  } else {
    const auto *VD = cast<VarDecl>(D);
    if (VD->isThisDeclarationADefinition() && VD->isExternallyVisible()) {
      S.Diag(AL.getLoc(), diag::err_alias_is_definition) << VD << 0;
      return;
    }
  }

  markUsedForAliasOrIfunc(S, D, AL, Str);
  D->addAttr(::new (S.Context) AliasAttr(S.Context, AL, Str));
}

static void handleTLSModelAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  StringRef Model;
  SourceLocation LiteralLoc;
  // Check that it is a string.
  if (!S.checkStringLiteralArgumentAttr(AL, 0, Model, &LiteralLoc))
    return;

  // Check that the value.
  if (Model != "global-dynamic" && Model != "local-dynamic"
      && Model != "initial-exec" && Model != "local-exec") {
    S.Diag(LiteralLoc, diag::err_attr_tlsmodel_arg);
    return;
  }

  D->addAttr(::new (S.Context) TLSModelAttr(S.Context, AL, Model));
}

static void handleRestrictAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  QualType ResultType = getFunctionOrMethodResultType(D);
  if (!ResultType->isAnyPointerType() && !ResultType->isBlockPointerType()) {
    S.Diag(AL.getLoc(), diag::warn_attribute_return_pointers_only)
        << AL << getFunctionOrMethodResultSourceRange(D);
    return;
  }

  if (AL.getNumArgs() == 0) {
    D->addAttr(::new (S.Context) RestrictAttr(S.Context, AL));
    return;
  }

  if (AL.getAttributeSpellingListIndex() == RestrictAttr::Declspec_restrict) {
    // __declspec(restrict) accepts no arguments
    S.Diag(AL.getLoc(), diag::err_attribute_wrong_number_arguments) << AL << 0;
    return;
  }

  // [[gnu::malloc(deallocator)]] with args specifies a deallocator function
  Expr *DeallocE = AL.getArgAsExpr(0);
  SourceLocation DeallocLoc = DeallocE->getExprLoc();
  FunctionDecl *DeallocFD = nullptr;
  DeclarationNameInfo DeallocNI;

  if (auto *DRE = dyn_cast<DeclRefExpr>(DeallocE)) {
    DeallocFD = dyn_cast<FunctionDecl>(DRE->getDecl());
    DeallocNI = DRE->getNameInfo();
    if (!DeallocFD) {
      S.Diag(DeallocLoc, diag::err_attribute_malloc_arg_not_function)
          << 1 << DeallocNI.getName();
      return;
    }
  } else if (auto *ULE = dyn_cast<UnresolvedLookupExpr>(DeallocE)) {
    DeallocFD = S.ResolveSingleFunctionTemplateSpecialization(ULE, true);
    DeallocNI = ULE->getNameInfo();
    if (!DeallocFD) {
      S.Diag(DeallocLoc, diag::err_attribute_malloc_arg_not_function)
          << 2 << DeallocNI.getName();
      if (ULE->getType() == S.Context.OverloadTy)
        S.NoteAllOverloadCandidates(ULE);
      return;
    }
  } else {
    S.Diag(DeallocLoc, diag::err_attribute_malloc_arg_not_function) << 0;
    return;
  }

  // 2nd arg of [[gnu::malloc(deallocator, 2)]] with args specifies the param
  // of deallocator that deallocates the pointer (defaults to 1)
  ParamIdx DeallocPtrIdx;
  if (AL.getNumArgs() == 1) {
    DeallocPtrIdx = ParamIdx(1, DeallocFD);

    if (!DeallocPtrIdx.isValid() ||
        !getFunctionOrMethodParamType(DeallocFD, DeallocPtrIdx.getASTIndex())
             .getCanonicalType()
             ->isPointerType()) {
      S.Diag(DeallocLoc,
             diag::err_attribute_malloc_arg_not_function_with_pointer_arg)
          << DeallocNI.getName();
      return;
    }
  } else {
    if (!S.checkFunctionOrMethodParameterIndex(
            DeallocFD, AL, 2, AL.getArgAsExpr(1), DeallocPtrIdx,
            /* CanIndexImplicitThis=*/false))
      return;

    QualType DeallocPtrArgType =
        getFunctionOrMethodParamType(DeallocFD, DeallocPtrIdx.getASTIndex());
    if (!DeallocPtrArgType.getCanonicalType()->isPointerType()) {
      S.Diag(DeallocLoc,
             diag::err_attribute_malloc_arg_refers_to_non_pointer_type)
          << DeallocPtrIdx.getSourceIndex() << DeallocPtrArgType
          << DeallocNI.getName();
      return;
    }
  }

  // FIXME: we should add this attribute to Clang's AST, so that clang-analyzer
  // can use it, see -Wmismatched-dealloc in GCC for what we can do with this.
  S.Diag(AL.getLoc(), diag::warn_attribute_form_ignored) << AL;
  D->addAttr(::new (S.Context)
                 RestrictAttr(S.Context, AL, DeallocE, DeallocPtrIdx));
}

static void handleCPUSpecificAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // Ensure we don't combine these with themselves, since that causes some
  // confusing behavior.
  if (AL.getParsedKind() == ParsedAttr::AT_CPUDispatch) {
    if (checkAttrMutualExclusion<CPUSpecificAttr>(S, D, AL))
      return;

    if (const auto *Other = D->getAttr<CPUDispatchAttr>()) {
      S.Diag(AL.getLoc(), diag::err_disallowed_duplicate_attribute) << AL;
      S.Diag(Other->getLocation(), diag::note_conflicting_attribute);
      return;
    }
  } else if (AL.getParsedKind() == ParsedAttr::AT_CPUSpecific) {
    if (checkAttrMutualExclusion<CPUDispatchAttr>(S, D, AL))
      return;

    if (const auto *Other = D->getAttr<CPUSpecificAttr>()) {
      S.Diag(AL.getLoc(), diag::err_disallowed_duplicate_attribute) << AL;
      S.Diag(Other->getLocation(), diag::note_conflicting_attribute);
      return;
    }
  }

  FunctionDecl *FD = cast<FunctionDecl>(D);

  if (const auto *MD = dyn_cast<CXXMethodDecl>(D)) {
    if (MD->getParent()->isLambda()) {
      S.Diag(AL.getLoc(), diag::err_attribute_dll_lambda) << AL;
      return;
    }
  }

  if (!AL.checkAtLeastNumArgs(S, 1))
    return;

  SmallVector<IdentifierInfo *, 8> CPUs;
  for (unsigned ArgNo = 0; ArgNo < getNumAttributeArgs(AL); ++ArgNo) {
    if (!AL.isArgIdent(ArgNo)) {
      S.Diag(AL.getLoc(), diag::err_attribute_argument_type)
          << AL << AANT_ArgumentIdentifier;
      return;
    }

    IdentifierLoc *CPUArg = AL.getArgAsIdent(ArgNo);
    StringRef CPUName = CPUArg->getIdentifierInfo()->getName().trim();

    if (!S.Context.getTargetInfo().validateCPUSpecificCPUDispatch(CPUName)) {
      S.Diag(CPUArg->getLoc(), diag::err_invalid_cpu_specific_dispatch_value)
          << CPUName << (AL.getKind() == ParsedAttr::AT_CPUDispatch);
      return;
    }

    const TargetInfo &Target = S.Context.getTargetInfo();
    if (llvm::any_of(CPUs, [CPUName, &Target](const IdentifierInfo *Cur) {
          return Target.CPUSpecificManglingCharacter(CPUName) ==
                 Target.CPUSpecificManglingCharacter(Cur->getName());
        })) {
      S.Diag(AL.getLoc(), diag::warn_multiversion_duplicate_entries);
      return;
    }
    CPUs.push_back(CPUArg->getIdentifierInfo());
  }

  FD->setIsMultiVersion(true);
  if (AL.getKind() == ParsedAttr::AT_CPUSpecific)
    D->addAttr(::new (S.Context)
                   CPUSpecificAttr(S.Context, AL, CPUs.data(), CPUs.size()));
  else
    D->addAttr(::new (S.Context)
                   CPUDispatchAttr(S.Context, AL, CPUs.data(), CPUs.size()));
}

static void handleCommonAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (S.LangOpts.CPlusPlus) {
    S.Diag(AL.getLoc(), diag::err_attribute_not_supported_in_lang)
        << AL << AttributeLangSupport::Cpp;
    return;
  }

  D->addAttr(::new (S.Context) CommonAttr(S.Context, AL));
}

static void handleNakedAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (AL.isDeclspecAttribute()) {
    const auto &Triple = S.getASTContext().getTargetInfo().getTriple();
    const auto &Arch = Triple.getArch();
    if (Arch != llvm::Triple::x86 &&
        (Arch != llvm::Triple::arm && Arch != llvm::Triple::thumb)) {
      S.Diag(AL.getLoc(), diag::err_attribute_not_supported_on_arch)
          << AL << Triple.getArchName();
      return;
    }

    // This form is not allowed to be written on a member function (static or
    // nonstatic) when in Microsoft compatibility mode.
    if (S.getLangOpts().MSVCCompat && isa<CXXMethodDecl>(D)) {
      S.Diag(AL.getLoc(), diag::err_attribute_wrong_decl_type)
          << AL << AL.isRegularKeywordAttribute() << ExpectedNonMemberFunction;
      return;
    }
  }

  D->addAttr(::new (S.Context) NakedAttr(S.Context, AL));
}

// FIXME: This is a best-effort heuristic.
// Currently only handles single throw expressions (optionally with
// ExprWithCleanups). We could expand this to perform control-flow analysis for
// more complex patterns.
static bool isKnownToAlwaysThrow(const FunctionDecl *FD) {
  if (!FD->hasBody())
    return false;
  const Stmt *Body = FD->getBody();
  const Stmt *OnlyStmt = nullptr;

  if (const auto *Compound = dyn_cast<CompoundStmt>(Body)) {
    if (Compound->size() != 1)
      return false; // More than one statement, can't be known to always throw.
    OnlyStmt = *Compound->body_begin();
  } else {
    OnlyStmt = Body;
  }

  // Unwrap ExprWithCleanups if necessary.
  if (const auto *EWC = dyn_cast<ExprWithCleanups>(OnlyStmt)) {
    OnlyStmt = EWC->getSubExpr();
  }
  // Check if the only statement is a throw expression.
  return isa<CXXThrowExpr>(OnlyStmt);
}

void clang::inferNoReturnAttr(Sema &S, const Decl *D) {
  auto *FD = dyn_cast<FunctionDecl>(D);
  if (!FD)
    return;

  auto *NonConstFD = const_cast<FunctionDecl *>(FD);
  DiagnosticsEngine &Diags = S.getDiagnostics();
  if (Diags.isIgnored(diag::warn_falloff_nonvoid, FD->getLocation()) &&
      Diags.isIgnored(diag::warn_suggest_noreturn_function, FD->getLocation()))
    return;

  if (!FD->hasAttr<NoReturnAttr>() && !FD->hasAttr<InferredNoReturnAttr>() &&
      isKnownToAlwaysThrow(FD)) {
    NonConstFD->addAttr(InferredNoReturnAttr::CreateImplicit(S.Context));

    // Emit a diagnostic suggesting the function being marked [[noreturn]].
    S.Diag(FD->getLocation(), diag::warn_suggest_noreturn_function)
        << /*isFunction=*/0 << FD;
  }
}

static void handleNoReturnAttr(Sema &S, Decl *D, const ParsedAttr &Attrs) {
  if (hasDeclarator(D)) return;

  if (!isa<ObjCMethodDecl>(D)) {
    S.Diag(Attrs.getLoc(), diag::warn_attribute_wrong_decl_type)
        << Attrs << Attrs.isRegularKeywordAttribute()
        << ExpectedFunctionOrMethod;
    return;
  }

  D->addAttr(::new (S.Context) NoReturnAttr(S.Context, Attrs));
}

static void handleStandardNoReturnAttr(Sema &S, Decl *D, const ParsedAttr &A) {
  // The [[_Noreturn]] spelling is deprecated in C23, so if that was used,
  // issue an appropriate diagnostic. However, don't issue a diagnostic if the
  // attribute name comes from a macro expansion. We don't want to punish users
  // who write [[noreturn]] after including <stdnoreturn.h> (where 'noreturn'
  // is defined as a macro which expands to '_Noreturn').
  if (!S.getLangOpts().CPlusPlus &&
      A.getSemanticSpelling() == CXX11NoReturnAttr::C23_Noreturn &&
      !(A.getLoc().isMacroID() &&
        S.getSourceManager().isInSystemMacro(A.getLoc())))
    S.Diag(A.getLoc(), diag::warn_deprecated_noreturn_spelling) << A.getRange();

  D->addAttr(::new (S.Context) CXX11NoReturnAttr(S.Context, A));
}

static void handleNoCfCheckAttr(Sema &S, Decl *D, const ParsedAttr &Attrs) {
  if (!S.getLangOpts().CFProtectionBranch)
    S.Diag(Attrs.getLoc(), diag::warn_nocf_check_attribute_ignored);
  else
    handleSimpleAttribute<AnyX86NoCfCheckAttr>(S, D, Attrs);
}

bool Sema::CheckAttrNoArgs(const ParsedAttr &Attrs) {
  if (!Attrs.checkExactlyNumArgs(*this, 0)) {
    Attrs.setInvalid();
    return true;
  }

  return false;
}

bool Sema::CheckAttrTarget(const ParsedAttr &AL) {
  // Check whether the attribute is valid on the current target.
  if (!AL.existsInTarget(Context.getTargetInfo())) {
    if (AL.isRegularKeywordAttribute())
      Diag(AL.getLoc(), diag::err_keyword_not_supported_on_target);
    else
      DiagnoseUnknownAttribute(AL);
    AL.setInvalid();
    return true;
  }
  return false;
}

static void handleAnalyzerNoReturnAttr(Sema &S, Decl *D, const ParsedAttr &AL) {

  // The checking path for 'noreturn' and 'analyzer_noreturn' are different
  // because 'analyzer_noreturn' does not impact the type.
  if (!isFunctionOrMethodOrBlockForAttrSubject(D)) {
    ValueDecl *VD = dyn_cast<ValueDecl>(D);
    if (!VD || (!VD->getType()->isBlockPointerType() &&
                !VD->getType()->isFunctionPointerType())) {
      S.Diag(AL.getLoc(), AL.isStandardAttributeSyntax()
                              ? diag::err_attribute_wrong_decl_type
                              : diag::warn_attribute_wrong_decl_type)
          << AL << AL.isRegularKeywordAttribute()
          << ExpectedFunctionMethodOrBlock;
      return;
    }
  }

  D->addAttr(::new (S.Context) AnalyzerNoReturnAttr(S.Context, AL));
}

// PS3 PPU-specific.
static void handleVecReturnAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  /*
    Returning a Vector Class in Registers

    According to the PPU ABI specifications, a class with a single member of
    vector type is returned in memory when used as the return value of a
    function.
    This results in inefficient code when implementing vector classes. To return
    the value in a single vector register, add the vecreturn attribute to the
    class definition. This attribute is also applicable to struct types.

    Example:

    struct Vector
    {
      __vector float xyzw;
    } __attribute__((vecreturn));

    Vector Add(Vector lhs, Vector rhs)
    {
      Vector result;
      result.xyzw = vec_add(lhs.xyzw, rhs.xyzw);
      return result; // This will be returned in a register
    }
  */
  if (VecReturnAttr *A = D->getAttr<VecReturnAttr>()) {
    S.Diag(AL.getLoc(), diag::err_repeat_attribute) << A;
    return;
  }

  const auto *R = cast<RecordDecl>(D);
  int count = 0;

  if (!isa<CXXRecordDecl>(R)) {
    S.Diag(AL.getLoc(), diag::err_attribute_vecreturn_only_vector_member);
    return;
  }

  if (!cast<CXXRecordDecl>(R)->isPOD()) {
    S.Diag(AL.getLoc(), diag::err_attribute_vecreturn_only_pod_record);
    return;
  }

  for (const auto *I : R->fields()) {
    if ((count == 1) || !I->getType()->isVectorType()) {
      S.Diag(AL.getLoc(), diag::err_attribute_vecreturn_only_vector_member);
      return;
    }
    count++;
  }

  D->addAttr(::new (S.Context) VecReturnAttr(S.Context, AL));
}

static void handleDependencyAttr(Sema &S, Scope *Scope, Decl *D,
                                 const ParsedAttr &AL) {
  if (isa<ParmVarDecl>(D)) {
    // [[carries_dependency]] can only be applied to a parameter if it is a
    // parameter of a function declaration or lambda.
    if (!(Scope->getFlags() & clang::Scope::FunctionDeclarationScope)) {
      S.Diag(AL.getLoc(),
             diag::err_carries_dependency_param_not_function_decl);
      return;
    }
  }

  D->addAttr(::new (S.Context) CarriesDependencyAttr(S.Context, AL));
}

static void handleUnusedAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  bool IsCXX17Attr = AL.isCXX11Attribute() && !AL.getScopeName();

  // If this is spelled as the standard C++17 attribute, but not in C++17, warn
  // about using it as an extension.
  if (!S.getLangOpts().CPlusPlus17 && IsCXX17Attr)
    S.Diag(AL.getLoc(), diag::ext_cxx17_attr) << AL;

  D->addAttr(::new (S.Context) UnusedAttr(S.Context, AL));
}

static void handleConstructorAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  uint32_t priority = ConstructorAttr::DefaultPriority;
  if (S.getLangOpts().HLSL && AL.getNumArgs()) {
    S.Diag(AL.getLoc(), diag::err_hlsl_init_priority_unsupported);
    return;
  }
  if (AL.getNumArgs() &&
      !S.checkUInt32Argument(AL, AL.getArgAsExpr(0), priority))
    return;
  S.Diag(D->getLocation(), diag::warn_global_constructor)
      << D->getSourceRange();

  D->addAttr(::new (S.Context) ConstructorAttr(S.Context, AL, priority));
}

static void handleDestructorAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  uint32_t priority = DestructorAttr::DefaultPriority;
  if (AL.getNumArgs() &&
      !S.checkUInt32Argument(AL, AL.getArgAsExpr(0), priority))
    return;
  S.Diag(D->getLocation(), diag::warn_global_destructor) << D->getSourceRange();

  D->addAttr(::new (S.Context) DestructorAttr(S.Context, AL, priority));
}

template <typename AttrTy>
static void handleAttrWithMessage(Sema &S, Decl *D, const ParsedAttr &AL) {
  // Handle the case where the attribute has a text message.
  StringRef Str;
  if (AL.getNumArgs() == 1 && !S.checkStringLiteralArgumentAttr(AL, 0, Str))
    return;

  D->addAttr(::new (S.Context) AttrTy(S.Context, AL, Str));
}

static bool checkAvailabilityAttr(Sema &S, SourceRange Range,
                                  IdentifierInfo *Platform,
                                  VersionTuple Introduced,
                                  VersionTuple Deprecated,
                                  VersionTuple Obsoleted) {
  StringRef PlatformName
    = AvailabilityAttr::getPrettyPlatformName(Platform->getName());
  if (PlatformName.empty())
    PlatformName = Platform->getName();

  // Ensure that Introduced <= Deprecated <= Obsoleted (although not all
  // of these steps are needed).
  if (!Introduced.empty() && !Deprecated.empty() &&
      !(Introduced <= Deprecated)) {
    S.Diag(Range.getBegin(), diag::warn_availability_version_ordering)
      << 1 << PlatformName << Deprecated.getAsString()
      << 0 << Introduced.getAsString();
    return true;
  }

  if (!Introduced.empty() && !Obsoleted.empty() &&
      !(Introduced <= Obsoleted)) {
    S.Diag(Range.getBegin(), diag::warn_availability_version_ordering)
      << 2 << PlatformName << Obsoleted.getAsString()
      << 0 << Introduced.getAsString();
    return true;
  }

  if (!Deprecated.empty() && !Obsoleted.empty() &&
      !(Deprecated <= Obsoleted)) {
    S.Diag(Range.getBegin(), diag::warn_availability_version_ordering)
      << 2 << PlatformName << Obsoleted.getAsString()
      << 1 << Deprecated.getAsString();
    return true;
  }

  return false;
}

/// Check whether the two versions match.
///
/// If either version tuple is empty, then they are assumed to match. If
/// \p BeforeIsOkay is true, then \p X can be less than or equal to \p Y.
static bool versionsMatch(const VersionTuple &X, const VersionTuple &Y,
                          bool BeforeIsOkay) {
  if (X.empty() || Y.empty())
    return true;

  if (X == Y)
    return true;

  if (BeforeIsOkay && X < Y)
    return true;

  return false;
}

AvailabilityAttr *Sema::mergeAvailabilityAttr(
    NamedDecl *D, const AttributeCommonInfo &CI, IdentifierInfo *Platform,
    bool Implicit, VersionTuple Introduced, VersionTuple Deprecated,
    VersionTuple Obsoleted, bool IsUnavailable, StringRef Message,
    bool IsStrict, StringRef Replacement, AvailabilityMergeKind AMK,
    int Priority, IdentifierInfo *Environment) {
  VersionTuple MergedIntroduced = Introduced;
  VersionTuple MergedDeprecated = Deprecated;
  VersionTuple MergedObsoleted = Obsoleted;
  bool FoundAny = false;
  bool OverrideOrImpl = false;
  switch (AMK) {
  case AvailabilityMergeKind::None:
  case AvailabilityMergeKind::Redeclaration:
    OverrideOrImpl = false;
    break;

  case AvailabilityMergeKind::Override:
  case AvailabilityMergeKind::ProtocolImplementation:
  case AvailabilityMergeKind::OptionalProtocolImplementation:
    OverrideOrImpl = true;
    break;
  }

  if (D->hasAttrs()) {
    AttrVec &Attrs = D->getAttrs();
    for (unsigned i = 0, e = Attrs.size(); i != e;) {
      const auto *OldAA = dyn_cast<AvailabilityAttr>(Attrs[i]);
      if (!OldAA) {
        ++i;
        continue;
      }

      IdentifierInfo *OldPlatform = OldAA->getPlatform();
      if (OldPlatform != Platform) {
        ++i;
        continue;
      }

      IdentifierInfo *OldEnvironment = OldAA->getEnvironment();
      if (OldEnvironment != Environment) {
        ++i;
        continue;
      }

      // If there is an existing availability attribute for this platform that
      // has a lower priority use the existing one and discard the new
      // attribute.
      if (OldAA->getPriority() < Priority)
        return nullptr;

      // If there is an existing attribute for this platform that has a higher
      // priority than the new attribute then erase the old one and continue
      // processing the attributes.
      if (OldAA->getPriority() > Priority) {
        Attrs.erase(Attrs.begin() + i);
        --e;
        continue;
      }

      FoundAny = true;
      VersionTuple OldIntroduced = OldAA->getIntroduced();
      VersionTuple OldDeprecated = OldAA->getDeprecated();
      VersionTuple OldObsoleted = OldAA->getObsoleted();
      bool OldIsUnavailable = OldAA->getUnavailable();

      if (!versionsMatch(OldIntroduced, Introduced, OverrideOrImpl) ||
          !versionsMatch(Deprecated, OldDeprecated, OverrideOrImpl) ||
          !versionsMatch(Obsoleted, OldObsoleted, OverrideOrImpl) ||
          !(OldIsUnavailable == IsUnavailable ||
            (OverrideOrImpl && !OldIsUnavailable && IsUnavailable))) {
        if (OverrideOrImpl) {
          int Which = -1;
          VersionTuple FirstVersion;
          VersionTuple SecondVersion;
          if (!versionsMatch(OldIntroduced, Introduced, OverrideOrImpl)) {
            Which = 0;
            FirstVersion = OldIntroduced;
            SecondVersion = Introduced;
          } else if (!versionsMatch(Deprecated, OldDeprecated, OverrideOrImpl)) {
            Which = 1;
            FirstVersion = Deprecated;
            SecondVersion = OldDeprecated;
          } else if (!versionsMatch(Obsoleted, OldObsoleted, OverrideOrImpl)) {
            Which = 2;
            FirstVersion = Obsoleted;
            SecondVersion = OldObsoleted;
          }

          if (Which == -1) {
            Diag(OldAA->getLocation(),
                 diag::warn_mismatched_availability_override_unavail)
                << AvailabilityAttr::getPrettyPlatformName(Platform->getName())
                << (AMK == AvailabilityMergeKind::Override);
          } else if (Which != 1 && AMK == AvailabilityMergeKind::
                                              OptionalProtocolImplementation) {
            // Allow different 'introduced' / 'obsoleted' availability versions
            // on a method that implements an optional protocol requirement. It
            // makes less sense to allow this for 'deprecated' as the user can't
            // see if the method is 'deprecated' as 'respondsToSelector' will
            // still return true when the method is deprecated.
            ++i;
            continue;
          } else {
            Diag(OldAA->getLocation(),
                 diag::warn_mismatched_availability_override)
                << Which
                << AvailabilityAttr::getPrettyPlatformName(Platform->getName())
                << FirstVersion.getAsString() << SecondVersion.getAsString()
                << (AMK == AvailabilityMergeKind::Override);
          }
          if (AMK == AvailabilityMergeKind::Override)
            Diag(CI.getLoc(), diag::note_overridden_method);
          else
            Diag(CI.getLoc(), diag::note_protocol_method);
        } else {
          Diag(OldAA->getLocation(), diag::warn_mismatched_availability);
          Diag(CI.getLoc(), diag::note_previous_attribute);
        }

        Attrs.erase(Attrs.begin() + i);
        --e;
        continue;
      }

      VersionTuple MergedIntroduced2 = MergedIntroduced;
      VersionTuple MergedDeprecated2 = MergedDeprecated;
      VersionTuple MergedObsoleted2 = MergedObsoleted;

      if (MergedIntroduced2.empty())
        MergedIntroduced2 = OldIntroduced;
      if (MergedDeprecated2.empty())
        MergedDeprecated2 = OldDeprecated;
      if (MergedObsoleted2.empty())
        MergedObsoleted2 = OldObsoleted;

      if (checkAvailabilityAttr(*this, OldAA->getRange(), Platform,
                                MergedIntroduced2, MergedDeprecated2,
                                MergedObsoleted2)) {
        Attrs.erase(Attrs.begin() + i);
        --e;
        continue;
      }

      MergedIntroduced = MergedIntroduced2;
      MergedDeprecated = MergedDeprecated2;
      MergedObsoleted = MergedObsoleted2;
      ++i;
    }
  }

  if (FoundAny &&
      MergedIntroduced == Introduced &&
      MergedDeprecated == Deprecated &&
      MergedObsoleted == Obsoleted)
    return nullptr;

  // Only create a new attribute if !OverrideOrImpl, but we want to do
  // the checking.
  if (!checkAvailabilityAttr(*this, CI.getRange(), Platform, MergedIntroduced,
                             MergedDeprecated, MergedObsoleted) &&
      !OverrideOrImpl) {
    auto *Avail = ::new (Context) AvailabilityAttr(
        Context, CI, Platform, Introduced, Deprecated, Obsoleted, IsUnavailable,
        Message, IsStrict, Replacement, Priority, Environment);
    Avail->setImplicit(Implicit);
    return Avail;
  }
  return nullptr;
}

static void handleAvailabilityAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (isa<UsingDecl, UnresolvedUsingTypenameDecl, UnresolvedUsingValueDecl>(
          D)) {
    S.Diag(AL.getRange().getBegin(), diag::warn_deprecated_ignored_on_using)
        << AL;
    return;
  }

  if (!AL.checkExactlyNumArgs(S, 1))
    return;
  IdentifierLoc *Platform = AL.getArgAsIdent(0);

  IdentifierInfo *II = Platform->getIdentifierInfo();
  StringRef PrettyName = AvailabilityAttr::getPrettyPlatformName(II->getName());
  if (PrettyName.empty())
    S.Diag(Platform->getLoc(), diag::warn_availability_unknown_platform)
        << Platform->getIdentifierInfo();

  auto *ND = dyn_cast<NamedDecl>(D);
  if (!ND) // We warned about this already, so just return.
    return;

  AvailabilityChange Introduced = AL.getAvailabilityIntroduced();
  AvailabilityChange Deprecated = AL.getAvailabilityDeprecated();
  AvailabilityChange Obsoleted = AL.getAvailabilityObsoleted();

  const llvm::Triple::OSType PlatformOS = AvailabilityAttr::getOSType(
      AvailabilityAttr::canonicalizePlatformName(II->getName()));

  auto reportAndUpdateIfInvalidOS = [&](auto &InputVersion) -> void {
    const bool IsInValidRange =
        llvm::Triple::isValidVersionForOS(PlatformOS, InputVersion);
    // Canonicalize availability versions.
    auto CanonicalVersion = llvm::Triple::getCanonicalVersionForOS(
        PlatformOS, InputVersion, IsInValidRange);
    if (!IsInValidRange) {
      S.Diag(Platform->getLoc(), diag::warn_availability_invalid_os_version)
          << InputVersion.getAsString() << PrettyName;
      S.Diag(Platform->getLoc(),
             diag::note_availability_invalid_os_version_adjusted)
          << CanonicalVersion.getAsString();
    }
    InputVersion = CanonicalVersion;
  };

  if (PlatformOS != llvm::Triple::OSType::UnknownOS) {
    reportAndUpdateIfInvalidOS(Introduced.Version);
    reportAndUpdateIfInvalidOS(Deprecated.Version);
    reportAndUpdateIfInvalidOS(Obsoleted.Version);
  }

  bool IsUnavailable = AL.getUnavailableLoc().isValid();
  bool IsStrict = AL.getStrictLoc().isValid();
  StringRef Str;
  if (const auto *SE = dyn_cast_if_present<StringLiteral>(AL.getMessageExpr()))
    Str = SE->getString();
  StringRef Replacement;
  if (const auto *SE =
          dyn_cast_if_present<StringLiteral>(AL.getReplacementExpr()))
    Replacement = SE->getString();

  if (II->isStr("swift")) {
    if (Introduced.isValid() || Obsoleted.isValid() ||
        (!IsUnavailable && !Deprecated.isValid())) {
      S.Diag(AL.getLoc(),
             diag::warn_availability_swift_unavailable_deprecated_only);
      return;
    }
  }

  if (II->isStr("fuchsia")) {
    std::optional<unsigned> Min, Sub;
    if ((Min = Introduced.Version.getMinor()) ||
        (Sub = Introduced.Version.getSubminor())) {
      S.Diag(AL.getLoc(), diag::warn_availability_fuchsia_unavailable_minor);
      return;
    }
  }

  if (S.getLangOpts().HLSL && IsStrict)
    S.Diag(AL.getStrictLoc(), diag::err_availability_unexpected_parameter)
        << "strict" << /* HLSL */ 0;

  int PriorityModifier = AL.isPragmaClangAttribute()
                             ? Sema::AP_PragmaClangAttribute
                             : Sema::AP_Explicit;

  const IdentifierLoc *EnvironmentLoc = AL.getEnvironment();
  IdentifierInfo *IIEnvironment = nullptr;
  if (EnvironmentLoc) {
    if (S.getLangOpts().HLSL) {
      IIEnvironment = EnvironmentLoc->getIdentifierInfo();
      if (AvailabilityAttr::getEnvironmentType(
              EnvironmentLoc->getIdentifierInfo()->getName()) ==
          llvm::Triple::EnvironmentType::UnknownEnvironment)
        S.Diag(EnvironmentLoc->getLoc(),
               diag::warn_availability_unknown_environment)
            << EnvironmentLoc->getIdentifierInfo();
    } else {
      S.Diag(EnvironmentLoc->getLoc(),
             diag::err_availability_unexpected_parameter)
          << "environment" << /* C/C++ */ 1;
    }
  }

  AvailabilityAttr *NewAttr = S.mergeAvailabilityAttr(
      ND, AL, II, false /*Implicit*/, Introduced.Version, Deprecated.Version,
      Obsoleted.Version, IsUnavailable, Str, IsStrict, Replacement,
      AvailabilityMergeKind::None, PriorityModifier, IIEnvironment);
  if (NewAttr)
    D->addAttr(NewAttr);

  // Transcribe "ios" to "watchos" (and add a new attribute) if the versioning
  // matches before the start of the watchOS platform.
  if (S.Context.getTargetInfo().getTriple().isWatchOS()) {
    IdentifierInfo *NewII = nullptr;
    if (II->getName() == "ios")
      NewII = &S.Context.Idents.get("watchos");
    else if (II->getName() == "ios_app_extension")
      NewII = &S.Context.Idents.get("watchos_app_extension");

    if (NewII) {
      const auto *SDKInfo = S.getDarwinSDKInfoForAvailabilityChecking();
      const auto *IOSToWatchOSMapping =
          SDKInfo ? SDKInfo->getVersionMapping(
                        DarwinSDKInfo::OSEnvPair::iOStoWatchOSPair())
                  : nullptr;

      auto adjustWatchOSVersion =
          [IOSToWatchOSMapping](VersionTuple Version) -> VersionTuple {
        if (Version.empty())
          return Version;
        auto MinimumWatchOSVersion = VersionTuple(2, 0);

        if (IOSToWatchOSMapping) {
          if (auto MappedVersion = IOSToWatchOSMapping->map(
                  Version, MinimumWatchOSVersion, std::nullopt)) {
            return *MappedVersion;
          }
        }

        auto Major = Version.getMajor();
        auto NewMajor = Major;
        if (Major < 9)
          NewMajor = 0;
        else if (Major < 12)
          NewMajor = Major - 7;
        if (NewMajor >= 2) {
          if (Version.getMinor()) {
            if (Version.getSubminor())
              return VersionTuple(NewMajor, *Version.getMinor(),
                                  *Version.getSubminor());
            else
              return VersionTuple(NewMajor, *Version.getMinor());
          }
          return VersionTuple(NewMajor);
        }

        return MinimumWatchOSVersion;
      };

      auto NewIntroduced = adjustWatchOSVersion(Introduced.Version);
      auto NewDeprecated = adjustWatchOSVersion(Deprecated.Version);
      auto NewObsoleted = adjustWatchOSVersion(Obsoleted.Version);

      AvailabilityAttr *NewAttr = S.mergeAvailabilityAttr(
          ND, AL, NewII, true /*Implicit*/, NewIntroduced, NewDeprecated,
          NewObsoleted, IsUnavailable, Str, IsStrict, Replacement,
          AvailabilityMergeKind::None,
          PriorityModifier + Sema::AP_InferredFromOtherPlatform, IIEnvironment);
      if (NewAttr)
        D->addAttr(NewAttr);
    }
  } else if (S.Context.getTargetInfo().getTriple().isTvOS()) {
    // Transcribe "ios" to "tvos" (and add a new attribute) if the versioning
    // matches before the start of the tvOS platform.
    IdentifierInfo *NewII = nullptr;
    if (II->getName() == "ios")
      NewII = &S.Context.Idents.get("tvos");
    else if (II->getName() == "ios_app_extension")
      NewII = &S.Context.Idents.get("tvos_app_extension");

    if (NewII) {
      const auto *SDKInfo = S.getDarwinSDKInfoForAvailabilityChecking();
      const auto *IOSToTvOSMapping =
          SDKInfo ? SDKInfo->getVersionMapping(
                        DarwinSDKInfo::OSEnvPair::iOStoTvOSPair())
                  : nullptr;

      auto AdjustTvOSVersion =
          [IOSToTvOSMapping](VersionTuple Version) -> VersionTuple {
        if (Version.empty())
          return Version;

        if (IOSToTvOSMapping) {
          if (auto MappedVersion = IOSToTvOSMapping->map(
                  Version, VersionTuple(0, 0), std::nullopt)) {
            return *MappedVersion;
          }
        }
        return Version;
      };

      auto NewIntroduced = AdjustTvOSVersion(Introduced.Version);
      auto NewDeprecated = AdjustTvOSVersion(Deprecated.Version);
      auto NewObsoleted = AdjustTvOSVersion(Obsoleted.Version);

      AvailabilityAttr *NewAttr = S.mergeAvailabilityAttr(
          ND, AL, NewII, true /*Implicit*/, NewIntroduced, NewDeprecated,
          NewObsoleted, IsUnavailable, Str, IsStrict, Replacement,
          AvailabilityMergeKind::None,
          PriorityModifier + Sema::AP_InferredFromOtherPlatform, IIEnvironment);
      if (NewAttr)
        D->addAttr(NewAttr);
    }
  } else if (S.Context.getTargetInfo().getTriple().getOS() ==
                 llvm::Triple::IOS &&
             S.Context.getTargetInfo().getTriple().isMacCatalystEnvironment()) {
    auto GetSDKInfo = [&]() {
      return S.getDarwinSDKInfoForAvailabilityChecking(AL.getRange().getBegin(),
                                                       "macOS");
    };

    // Transcribe "ios" to "maccatalyst" (and add a new attribute).
    IdentifierInfo *NewII = nullptr;
    if (II->getName() == "ios")
      NewII = &S.Context.Idents.get("maccatalyst");
    else if (II->getName() == "ios_app_extension")
      NewII = &S.Context.Idents.get("maccatalyst_app_extension");
    if (NewII) {
      auto MinMacCatalystVersion = [](const VersionTuple &V) {
        if (V.empty())
          return V;
        if (V.getMajor() < 13 ||
            (V.getMajor() == 13 && V.getMinor() && *V.getMinor() < 1))
          return VersionTuple(13, 1); // The min Mac Catalyst version is 13.1.
        return V;
      };
      AvailabilityAttr *NewAttr = S.mergeAvailabilityAttr(
          ND, AL, NewII, true /*Implicit*/,
          MinMacCatalystVersion(Introduced.Version),
          MinMacCatalystVersion(Deprecated.Version),
          MinMacCatalystVersion(Obsoleted.Version), IsUnavailable, Str,
          IsStrict, Replacement, AvailabilityMergeKind::None,
          PriorityModifier + Sema::AP_InferredFromOtherPlatform, IIEnvironment);
      if (NewAttr)
        D->addAttr(NewAttr);
    } else if (II->getName() == "macos" && GetSDKInfo() &&
               (!Introduced.Version.empty() || !Deprecated.Version.empty() ||
                !Obsoleted.Version.empty())) {
      if (const auto *MacOStoMacCatalystMapping =
              GetSDKInfo()->getVersionMapping(
                  DarwinSDKInfo::OSEnvPair::macOStoMacCatalystPair())) {
        // Infer Mac Catalyst availability from the macOS availability attribute
        // if it has versioned availability. Don't infer 'unavailable'. This
        // inferred availability has lower priority than the other availability
        // attributes that are inferred from 'ios'.
        NewII = &S.Context.Idents.get("maccatalyst");
        auto RemapMacOSVersion =
            [&](const VersionTuple &V) -> std::optional<VersionTuple> {
          if (V.empty())
            return std::nullopt;
          // API_TO_BE_DEPRECATED is 100000.
          if (V.getMajor() == 100000)
            return VersionTuple(100000);
          // The minimum iosmac version is 13.1
          return MacOStoMacCatalystMapping->map(V, VersionTuple(13, 1),
                                                std::nullopt);
        };
        std::optional<VersionTuple> NewIntroduced =
                                        RemapMacOSVersion(Introduced.Version),
                                    NewDeprecated =
                                        RemapMacOSVersion(Deprecated.Version),
                                    NewObsoleted =
                                        RemapMacOSVersion(Obsoleted.Version);
        if (NewIntroduced || NewDeprecated || NewObsoleted) {
          auto VersionOrEmptyVersion =
              [](const std::optional<VersionTuple> &V) -> VersionTuple {
            return V ? *V : VersionTuple();
          };
          AvailabilityAttr *NewAttr = S.mergeAvailabilityAttr(
              ND, AL, NewII, true /*Implicit*/,
              VersionOrEmptyVersion(NewIntroduced),
              VersionOrEmptyVersion(NewDeprecated),
              VersionOrEmptyVersion(NewObsoleted), /*IsUnavailable=*/false, Str,
              IsStrict, Replacement, AvailabilityMergeKind::None,
              PriorityModifier + Sema::AP_InferredFromOtherPlatform +
                  Sema::AP_InferredFromOtherPlatform,
              IIEnvironment);
          if (NewAttr)
            D->addAttr(NewAttr);
        }
      }
    }
  }
}

static void handleExternalSourceSymbolAttr(Sema &S, Decl *D,
                                           const ParsedAttr &AL) {
  if (!AL.checkAtLeastNumArgs(S, 1) || !AL.checkAtMostNumArgs(S, 4))
    return;

  StringRef Language;
  if (const auto *SE = dyn_cast_if_present<StringLiteral>(AL.getArgAsExpr(0)))
    Language = SE->getString();
  StringRef DefinedIn;
  if (const auto *SE = dyn_cast_if_present<StringLiteral>(AL.getArgAsExpr(1)))
    DefinedIn = SE->getString();
  bool IsGeneratedDeclaration = AL.getArgAsIdent(2) != nullptr;
  StringRef USR;
  if (const auto *SE = dyn_cast_if_present<StringLiteral>(AL.getArgAsExpr(3)))
    USR = SE->getString();

  D->addAttr(::new (S.Context) ExternalSourceSymbolAttr(
      S.Context, AL, Language, DefinedIn, IsGeneratedDeclaration, USR));
}

template <class T>
static T *mergeVisibilityAttr(Sema &S, Decl *D, const AttributeCommonInfo &CI,
                              typename T::VisibilityType value) {
  T *existingAttr = D->getAttr<T>();
  if (existingAttr) {
    typename T::VisibilityType existingValue = existingAttr->getVisibility();
    if (existingValue == value)
      return nullptr;
    S.Diag(existingAttr->getLocation(), diag::err_mismatched_visibility);
    S.Diag(CI.getLoc(), diag::note_previous_attribute);
    D->dropAttr<T>();
  }
  return ::new (S.Context) T(S.Context, CI, value);
}

VisibilityAttr *Sema::mergeVisibilityAttr(Decl *D,
                                          const AttributeCommonInfo &CI,
                                          VisibilityAttr::VisibilityType Vis) {
  return ::mergeVisibilityAttr<VisibilityAttr>(*this, D, CI, Vis);
}

TypeVisibilityAttr *
Sema::mergeTypeVisibilityAttr(Decl *D, const AttributeCommonInfo &CI,
                              TypeVisibilityAttr::VisibilityType Vis) {
  return ::mergeVisibilityAttr<TypeVisibilityAttr>(*this, D, CI, Vis);
}

static void handleVisibilityAttr(Sema &S, Decl *D, const ParsedAttr &AL,
                                 bool isTypeVisibility) {
  // Visibility attributes don't mean anything on a typedef.
  if (isa<TypedefNameDecl>(D)) {
    S.Diag(AL.getRange().getBegin(), diag::warn_attribute_ignored) << AL;
    return;
  }

  // 'type_visibility' can only go on a type or namespace.
  if (isTypeVisibility && !(isa<TagDecl>(D) || isa<ObjCInterfaceDecl>(D) ||
                            isa<NamespaceDecl>(D))) {
    S.Diag(AL.getRange().getBegin(), diag::err_attribute_wrong_decl_type)
        << AL << AL.isRegularKeywordAttribute() << ExpectedTypeOrNamespace;
    return;
  }

  // Check that the argument is a string literal.
  StringRef TypeStr;
  SourceLocation LiteralLoc;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, TypeStr, &LiteralLoc))
    return;

  VisibilityAttr::VisibilityType type;
  if (!VisibilityAttr::ConvertStrToVisibilityType(TypeStr, type)) {
    S.Diag(LiteralLoc, diag::warn_attribute_type_not_supported) << AL
                                                                << TypeStr;
    return;
  }

  // Complain about attempts to use protected visibility on targets
  // (like Darwin) that don't support it.
  if (type == VisibilityAttr::Protected &&
      !S.Context.getTargetInfo().hasProtectedVisibility()) {
    S.Diag(AL.getLoc(), diag::warn_attribute_protected_visibility);
    type = VisibilityAttr::Default;
  }

  Attr *newAttr;
  if (isTypeVisibility) {
    newAttr = S.mergeTypeVisibilityAttr(
        D, AL, (TypeVisibilityAttr::VisibilityType)type);
  } else {
    newAttr = S.mergeVisibilityAttr(D, AL, type);
  }
  if (newAttr)
    D->addAttr(newAttr);
}

static void handleSentinelAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  unsigned sentinel = (unsigned)SentinelAttr::DefaultSentinel;
  if (AL.getNumArgs() > 0) {
    Expr *E = AL.getArgAsExpr(0);
    std::optional<llvm::APSInt> Idx = llvm::APSInt(32);
    if (E->isTypeDependent() || !(Idx = E->getIntegerConstantExpr(S.Context))) {
      S.Diag(AL.getLoc(), diag::err_attribute_argument_n_type)
          << AL << 1 << AANT_ArgumentIntegerConstant << E->getSourceRange();
      return;
    }

    if (Idx->isSigned() && Idx->isNegative()) {
      S.Diag(AL.getLoc(), diag::err_attribute_sentinel_less_than_zero)
        << E->getSourceRange();
      return;
    }

    sentinel = Idx->getZExtValue();
  }

  unsigned nullPos = (unsigned)SentinelAttr::DefaultNullPos;
  if (AL.getNumArgs() > 1) {
    Expr *E = AL.getArgAsExpr(1);
    std::optional<llvm::APSInt> Idx = llvm::APSInt(32);
    if (E->isTypeDependent() || !(Idx = E->getIntegerConstantExpr(S.Context))) {
      S.Diag(AL.getLoc(), diag::err_attribute_argument_n_type)
          << AL << 2 << AANT_ArgumentIntegerConstant << E->getSourceRange();
      return;
    }
    nullPos = Idx->getZExtValue();

    if ((Idx->isSigned() && Idx->isNegative()) || nullPos > 1) {
      // FIXME: This error message could be improved, it would be nice
      // to say what the bounds actually are.
      S.Diag(AL.getLoc(), diag::err_attribute_sentinel_not_zero_or_one)
        << E->getSourceRange();
      return;
    }
  }

  if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
    const FunctionType *FT = FD->getType()->castAs<FunctionType>();
    if (isa<FunctionNoProtoType>(FT)) {
      S.Diag(AL.getLoc(), diag::warn_attribute_sentinel_named_arguments);
      return;
    }

    if (!cast<FunctionProtoType>(FT)->isVariadic()) {
      S.Diag(AL.getLoc(), diag::warn_attribute_sentinel_not_variadic) << 0;
      return;
    }
  } else if (const auto *MD = dyn_cast<ObjCMethodDecl>(D)) {
    if (!MD->isVariadic()) {
      S.Diag(AL.getLoc(), diag::warn_attribute_sentinel_not_variadic) << 0;
      return;
    }
  } else if (const auto *BD = dyn_cast<BlockDecl>(D)) {
    if (!BD->isVariadic()) {
      S.Diag(AL.getLoc(), diag::warn_attribute_sentinel_not_variadic) << 1;
      return;
    }
  } else if (const auto *V = dyn_cast<VarDecl>(D)) {
    QualType Ty = V->getType();
    if (Ty->isBlockPointerType() || Ty->isFunctionPointerType()) {
      const FunctionType *FT = Ty->isFunctionPointerType()
                                   ? D->getFunctionType()
                                   : Ty->castAs<BlockPointerType>()
                                         ->getPointeeType()
                                         ->castAs<FunctionType>();
      if (!cast<FunctionProtoType>(FT)->isVariadic()) {
        int m = Ty->isFunctionPointerType() ? 0 : 1;
        S.Diag(AL.getLoc(), diag::warn_attribute_sentinel_not_variadic) << m;
        return;
      }
    } else {
      S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type)
          << AL << AL.isRegularKeywordAttribute()
          << ExpectedFunctionMethodOrBlock;
      return;
    }
  } else {
    S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type)
        << AL << AL.isRegularKeywordAttribute()
        << ExpectedFunctionMethodOrBlock;
    return;
  }
  D->addAttr(::new (S.Context) SentinelAttr(S.Context, AL, sentinel, nullPos));
}

static void handleWarnUnusedResult(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (D->getFunctionType() &&
      D->getFunctionType()->getReturnType()->isVoidType() &&
      !isa<CXXConstructorDecl>(D)) {
    S.Diag(AL.getLoc(), diag::warn_attribute_void_function_method) << AL << 0;
    return;
  }
  if (const auto *MD = dyn_cast<ObjCMethodDecl>(D))
    if (MD->getReturnType()->isVoidType()) {
      S.Diag(AL.getLoc(), diag::warn_attribute_void_function_method) << AL << 1;
      return;
    }

  StringRef Str;
  if (AL.isStandardAttributeSyntax() && !AL.getScopeName()) {
    // The standard attribute cannot be applied to variable declarations such
    // as a function pointer.
    if (isa<VarDecl>(D))
      S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type)
          << AL << AL.isRegularKeywordAttribute()
          << ExpectedFunctionOrClassOrEnum;

    // If this is spelled as the standard C++17 attribute, but not in C++17,
    // warn about using it as an extension. If there are attribute arguments,
    // then claim it's a C++20 extension instead. C23 supports this attribute
    // with the message; no extension warning is needed there beyond the one
    // already issued for accepting attributes in older modes.
    const LangOptions &LO = S.getLangOpts();
    if (AL.getNumArgs() == 1) {
      if (LO.CPlusPlus && !LO.CPlusPlus20)
        S.Diag(AL.getLoc(), diag::ext_cxx20_attr) << AL;

      if (!S.checkStringLiteralArgumentAttr(AL, 0, Str, nullptr))
        return;
    } else if (LO.CPlusPlus && !LO.CPlusPlus17)
      S.Diag(AL.getLoc(), diag::ext_cxx17_attr) << AL;
  }

  if ((!AL.isGNUAttribute() &&
       !(AL.isStandardAttributeSyntax() && AL.isClangScope())) &&
      isa<TypedefNameDecl>(D)) {
    S.Diag(AL.getLoc(), diag::warn_unused_result_typedef_unsupported_spelling)
        << AL.isGNUScope();
    return;
  }

  D->addAttr(::new (S.Context) WarnUnusedResultAttr(S.Context, AL, Str));
}

static void handleWeakImportAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // weak_import only applies to variable & function declarations.
  bool isDef = false;
  if (!D->canBeWeakImported(isDef)) {
    if (isDef)
      S.Diag(AL.getLoc(), diag::warn_attribute_invalid_on_definition)
        << "weak_import";
    else if (isa<ObjCPropertyDecl>(D) || isa<ObjCMethodDecl>(D) ||
             (S.Context.getTargetInfo().getTriple().isOSDarwin() &&
              (isa<ObjCInterfaceDecl>(D) || isa<EnumDecl>(D)))) {
      // Nothing to warn about here.
    } else
      S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type)
          << AL << AL.isRegularKeywordAttribute() << ExpectedVariableOrFunction;

    return;
  }

  D->addAttr(::new (S.Context) WeakImportAttr(S.Context, AL));
}

// Checks whether an argument of launch_bounds-like attribute is
// acceptable, performs implicit conversion to Rvalue, and returns
// non-nullptr Expr result on success. Otherwise, it returns nullptr
// and may output an error.
template <class Attribute>
static Expr *makeAttributeArgExpr(Sema &S, Expr *E, const Attribute &Attr,
                                  const unsigned Idx) {
  if (S.DiagnoseUnexpandedParameterPack(E))
    return nullptr;

  // Accept template arguments for now as they depend on something else.
  // We'll get to check them when they eventually get instantiated.
  if (E->isValueDependent())
    return E;

  std::optional<llvm::APSInt> I = llvm::APSInt(64);
  if (!(I = E->getIntegerConstantExpr(S.Context))) {
    S.Diag(E->getExprLoc(), diag::err_attribute_argument_n_type)
        << &Attr << Idx << AANT_ArgumentIntegerConstant << E->getSourceRange();
    return nullptr;
  }
  // Make sure we can fit it in 32 bits.
  if (!I->isIntN(32)) {
    S.Diag(E->getExprLoc(), diag::err_ice_too_large)
        << toString(*I, 10, false) << 32 << /* Unsigned */ 1;
    return nullptr;
  }
  if (*I < 0)
    S.Diag(E->getExprLoc(), diag::err_attribute_requires_positive_integer)
        << &Attr << /*non-negative*/ 1 << E->getSourceRange();

  // We may need to perform implicit conversion of the argument.
  InitializedEntity Entity = InitializedEntity::InitializeParameter(
      S.Context, S.Context.getConstType(S.Context.IntTy), /*consume*/ false);
  ExprResult ValArg = S.PerformCopyInitialization(Entity, SourceLocation(), E);
  assert(!ValArg.isInvalid() &&
         "Unexpected PerformCopyInitialization() failure.");

  return ValArg.getAs<Expr>();
}

// Handles reqd_work_group_size and work_group_size_hint.
template <typename WorkGroupAttr>
static void handleWorkGroupSize(Sema &S, Decl *D, const ParsedAttr &AL) {
  Expr *WGSize[3];
  for (unsigned i = 0; i < 3; ++i) {
    if (Expr *E = makeAttributeArgExpr(S, AL.getArgAsExpr(i), AL, i))
      WGSize[i] = E;
    else
      return;
  }

  auto IsZero = [&](Expr *E) {
    if (E->isValueDependent())
      return false;
    std::optional<llvm::APSInt> I = E->getIntegerConstantExpr(S.Context);
    assert(I && "Non-integer constant expr");
    return I->isZero();
  };

  if (!llvm::all_of(WGSize, IsZero)) {
    for (unsigned i = 0; i < 3; ++i) {
      const Expr *E = AL.getArgAsExpr(i);
      if (IsZero(WGSize[i])) {
        S.Diag(AL.getLoc(), diag::err_attribute_argument_is_zero)
            << AL << E->getSourceRange();
        return;
      }
    }
  }

  auto Equal = [&](Expr *LHS, Expr *RHS) {
    if (LHS->isValueDependent() || RHS->isValueDependent())
      return true;
    std::optional<llvm::APSInt> L = LHS->getIntegerConstantExpr(S.Context);
    assert(L && "Non-integer constant expr");
    std::optional<llvm::APSInt> R = RHS->getIntegerConstantExpr(S.Context);
    assert(L && "Non-integer constant expr");
    return L == R;
  };

  WorkGroupAttr *Existing = D->getAttr<WorkGroupAttr>();
  if (Existing &&
      !llvm::equal(std::initializer_list<Expr *>{Existing->getXDim(),
                                                 Existing->getYDim(),
                                                 Existing->getZDim()},
                   WGSize, Equal))
    S.Diag(AL.getLoc(), diag::warn_duplicate_attribute) << AL;

  D->addAttr(::new (S.Context)
                 WorkGroupAttr(S.Context, AL, WGSize[0], WGSize[1], WGSize[2]));
}

static void handleVecTypeHint(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!AL.hasParsedType()) {
    S.Diag(AL.getLoc(), diag::err_attribute_wrong_number_arguments) << AL << 1;
    return;
  }

  TypeSourceInfo *ParmTSI = nullptr;
  QualType ParmType = S.GetTypeFromParser(AL.getTypeArg(), &ParmTSI);
  assert(ParmTSI && "no type source info for attribute argument");

  if (!ParmType->isExtVectorType() && !ParmType->isFloatingType() &&
      (ParmType->isBooleanType() ||
       !ParmType->isIntegralType(S.getASTContext()))) {
    S.Diag(AL.getLoc(), diag::err_attribute_invalid_argument) << 2 << AL;
    return;
  }

  if (VecTypeHintAttr *A = D->getAttr<VecTypeHintAttr>()) {
    if (!S.Context.hasSameType(A->getTypeHint(), ParmType)) {
      S.Diag(AL.getLoc(), diag::warn_duplicate_attribute) << AL;
      return;
    }
  }

  D->addAttr(::new (S.Context) VecTypeHintAttr(S.Context, AL, ParmTSI));
}

SectionAttr *Sema::mergeSectionAttr(Decl *D, const AttributeCommonInfo &CI,
                                    StringRef Name) {
  // Explicit or partial specializations do not inherit
  // the section attribute from the primary template.
  if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
    if (CI.getAttributeSpellingListIndex() == SectionAttr::Declspec_allocate &&
        FD->isFunctionTemplateSpecialization())
      return nullptr;
  }
  if (SectionAttr *ExistingAttr = D->getAttr<SectionAttr>()) {
    if (ExistingAttr->getName() == Name)
      return nullptr;
    Diag(ExistingAttr->getLocation(), diag::warn_mismatched_section)
         << 1 /*section*/;
    Diag(CI.getLoc(), diag::note_previous_attribute);
    return nullptr;
  }
  return ::new (Context) SectionAttr(Context, CI, Name);
}

llvm::Error Sema::isValidSectionSpecifier(StringRef SecName) {
  if (!Context.getTargetInfo().getTriple().isOSDarwin())
    return llvm::Error::success();

  // Let MCSectionMachO validate this.
  StringRef Segment, Section;
  unsigned TAA, StubSize;
  bool HasTAA;
  return llvm::MCSectionMachO::ParseSectionSpecifier(SecName, Segment, Section,
                                                     TAA, HasTAA, StubSize);
}

bool Sema::checkSectionName(SourceLocation LiteralLoc, StringRef SecName) {
  if (llvm::Error E = isValidSectionSpecifier(SecName)) {
    Diag(LiteralLoc, diag::err_attribute_section_invalid_for_target)
        << toString(std::move(E)) << 1 /*'section'*/;
    return false;
  }
  return true;
}

static void handleSectionAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // Make sure that there is a string literal as the sections's single
  // argument.
  StringRef Str;
  SourceLocation LiteralLoc;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, Str, &LiteralLoc))
    return;

  if (!S.checkSectionName(LiteralLoc, Str))
    return;

  SectionAttr *NewAttr = S.mergeSectionAttr(D, AL, Str);
  if (NewAttr) {
    D->addAttr(NewAttr);
    if (isa<FunctionDecl, FunctionTemplateDecl, ObjCMethodDecl,
            ObjCPropertyDecl>(D))
      S.UnifySection(NewAttr->getName(),
                     ASTContext::PSF_Execute | ASTContext::PSF_Read,
                     cast<NamedDecl>(D));
  }
}

static bool isValidCodeModelAttr(llvm::Triple &Triple, StringRef Str) {
  if (Triple.isLoongArch()) {
    return Str == "normal" || Str == "medium" || Str == "extreme";
  } else {
    assert(Triple.getArch() == llvm::Triple::x86_64 &&
           "only loongarch/x86-64 supported");
    return Str == "small" || Str == "large";
  }
}

static void handleCodeModelAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  StringRef Str;
  SourceLocation LiteralLoc;
  auto IsTripleSupported = [](llvm::Triple &Triple) {
    return Triple.getArch() == llvm::Triple::ArchType::x86_64 ||
           Triple.isLoongArch();
  };

  // Check that it is a string.
  if (!S.checkStringLiteralArgumentAttr(AL, 0, Str, &LiteralLoc))
    return;

  SmallVector<llvm::Triple, 2> Triples = {
      S.Context.getTargetInfo().getTriple()};
  if (auto *aux = S.Context.getAuxTargetInfo()) {
    Triples.push_back(aux->getTriple());
  } else if (S.Context.getTargetInfo().getTriple().isNVPTX() ||
             S.Context.getTargetInfo().getTriple().isAMDGPU() ||
             S.Context.getTargetInfo().getTriple().isSPIRV()) {
    // Ignore the attribute for pure GPU device compiles since it only applies
    // to host globals.
    return;
  }

  auto SupportedTripleIt = llvm::find_if(Triples, IsTripleSupported);
  if (SupportedTripleIt == Triples.end()) {
    S.Diag(LiteralLoc, diag::warn_unknown_attribute_ignored) << AL;
    return;
  }

  llvm::CodeModel::Model CM;
  if (!CodeModelAttr::ConvertStrToModel(Str, CM) ||
      !isValidCodeModelAttr(*SupportedTripleIt, Str)) {
    S.Diag(LiteralLoc, diag::err_attr_codemodel_arg) << Str;
    return;
  }

  D->addAttr(::new (S.Context) CodeModelAttr(S.Context, AL, CM));
}

// This is used for `__declspec(code_seg("segname"))` on a decl.
// `#pragma code_seg("segname")` uses checkSectionName() instead.
static bool checkCodeSegName(Sema &S, SourceLocation LiteralLoc,
                             StringRef CodeSegName) {
  if (llvm::Error E = S.isValidSectionSpecifier(CodeSegName)) {
    S.Diag(LiteralLoc, diag::err_attribute_section_invalid_for_target)
        << toString(std::move(E)) << 0 /*'code-seg'*/;
    return false;
  }

  return true;
}

CodeSegAttr *Sema::mergeCodeSegAttr(Decl *D, const AttributeCommonInfo &CI,
                                    StringRef Name) {
  // Explicit or partial specializations do not inherit
  // the code_seg attribute from the primary template.
  if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
    if (FD->isFunctionTemplateSpecialization())
      return nullptr;
  }
  if (const auto *ExistingAttr = D->getAttr<CodeSegAttr>()) {
    if (ExistingAttr->getName() == Name)
      return nullptr;
    Diag(ExistingAttr->getLocation(), diag::warn_mismatched_section)
         << 0 /*codeseg*/;
    Diag(CI.getLoc(), diag::note_previous_attribute);
    return nullptr;
  }
  return ::new (Context) CodeSegAttr(Context, CI, Name);
}

static void handleCodeSegAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  StringRef Str;
  SourceLocation LiteralLoc;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, Str, &LiteralLoc))
    return;
  if (!checkCodeSegName(S, LiteralLoc, Str))
    return;
  if (const auto *ExistingAttr = D->getAttr<CodeSegAttr>()) {
    if (!ExistingAttr->isImplicit()) {
      S.Diag(AL.getLoc(),
             ExistingAttr->getName() == Str
             ? diag::warn_duplicate_codeseg_attribute
             : diag::err_conflicting_codeseg_attribute);
      return;
    }
    D->dropAttr<CodeSegAttr>();
  }
  if (CodeSegAttr *CSA = S.mergeCodeSegAttr(D, AL, Str))
    D->addAttr(CSA);
}

bool Sema::checkTargetAttr(SourceLocation LiteralLoc, StringRef AttrStr) {
  enum FirstParam { Unsupported, Duplicate, Unknown };
  enum SecondParam { None, CPU, Tune };
  enum ThirdParam { Target, TargetClones };
  if (AttrStr.contains("fpmath="))
    return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
           << Unsupported << None << "fpmath=" << Target;

  // Diagnose use of tune if target doesn't support it.
  if (!Context.getTargetInfo().supportsTargetAttributeTune() &&
      AttrStr.contains("tune="))
    return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
           << Unsupported << None << "tune=" << Target;

  ParsedTargetAttr ParsedAttrs =
      Context.getTargetInfo().parseTargetAttr(AttrStr);

  if (!ParsedAttrs.CPU.empty() &&
      !Context.getTargetInfo().isValidCPUName(ParsedAttrs.CPU))
    return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
           << Unknown << CPU << ParsedAttrs.CPU << Target;

  if (!ParsedAttrs.Tune.empty() &&
      !Context.getTargetInfo().isValidCPUName(ParsedAttrs.Tune))
    return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
           << Unknown << Tune << ParsedAttrs.Tune << Target;

  if (Context.getTargetInfo().getTriple().isRISCV()) {
    if (ParsedAttrs.Duplicate != "")
      return Diag(LiteralLoc, diag::err_duplicate_target_attribute)
             << Duplicate << None << ParsedAttrs.Duplicate << Target;
    for (StringRef CurFeature : ParsedAttrs.Features) {
      if (!CurFeature.starts_with('+') && !CurFeature.starts_with('-'))
        return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
               << Unsupported << None << AttrStr << Target;
    }
  }

  if (Context.getTargetInfo().getTriple().isLoongArch()) {
    for (StringRef CurFeature : ParsedAttrs.Features) {
      if (CurFeature.starts_with("!arch=")) {
        StringRef ArchValue = CurFeature.split("=").second.trim();
        return Diag(LiteralLoc, diag::err_attribute_unsupported)
               << "target(arch=..)" << ArchValue;
      }
    }
  }

  if (ParsedAttrs.Duplicate != "")
    return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
           << Duplicate << None << ParsedAttrs.Duplicate << Target;

  for (const auto &Feature : ParsedAttrs.Features) {
    auto CurFeature = StringRef(Feature).drop_front(); // remove + or -.
    if (!Context.getTargetInfo().isValidFeatureName(CurFeature))
      return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
             << Unsupported << None << CurFeature << Target;
  }

  TargetInfo::BranchProtectionInfo BPI{};
  StringRef DiagMsg;
  if (ParsedAttrs.BranchProtection.empty())
    return false;
  if (!Context.getTargetInfo().validateBranchProtection(
          ParsedAttrs.BranchProtection, ParsedAttrs.CPU, BPI,
          Context.getLangOpts(), DiagMsg)) {
    if (DiagMsg.empty())
      return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
             << Unsupported << None << "branch-protection" << Target;
    return Diag(LiteralLoc, diag::err_invalid_branch_protection_spec)
           << DiagMsg;
  }
  if (!DiagMsg.empty())
    Diag(LiteralLoc, diag::warn_unsupported_branch_protection_spec) << DiagMsg;

  return false;
}

bool Sema::checkTargetVersionAttr(SourceLocation LiteralLoc, Decl *D,
                                  StringRef AttrStr) {
  enum FirstParam { Unsupported };
  enum SecondParam { None };
  enum ThirdParam { Target, TargetClones, TargetVersion };
  llvm::SmallVector<StringRef, 8> Features;
  if (Context.getTargetInfo().getTriple().isRISCV()) {
    llvm::SmallVector<StringRef, 8> AttrStrs;
    AttrStr.split(AttrStrs, ';');

    bool HasArch = false;
    bool HasPriority = false;
    bool HasDefault = false;
    bool DuplicateAttr = false;
    for (auto &AttrStr : AttrStrs) {
      // Only support arch=+ext,... syntax.
      if (AttrStr.starts_with("arch=+")) {
        if (HasArch)
          DuplicateAttr = true;
        HasArch = true;
        ParsedTargetAttr TargetAttr =
            Context.getTargetInfo().parseTargetAttr(AttrStr);

        if (TargetAttr.Features.empty() ||
            llvm::any_of(TargetAttr.Features, [&](const StringRef Ext) {
              return !RISCV().isValidFMVExtension(Ext);
            }))
          return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
                 << Unsupported << None << AttrStr << TargetVersion;
      } else if (AttrStr.starts_with("default")) {
        if (HasDefault)
          DuplicateAttr = true;
        HasDefault = true;
      } else if (AttrStr.consume_front("priority=")) {
        if (HasPriority)
          DuplicateAttr = true;
        HasPriority = true;
        unsigned Digit;
        if (AttrStr.getAsInteger(0, Digit))
          return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
                 << Unsupported << None << AttrStr << TargetVersion;
      } else {
        return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
               << Unsupported << None << AttrStr << TargetVersion;
      }
    }

    if (((HasPriority || HasArch) && HasDefault) || DuplicateAttr ||
        (HasPriority && !HasArch))
      return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
             << Unsupported << None << AttrStr << TargetVersion;

    return false;
  }
  AttrStr.split(Features, "+");
  for (auto &CurFeature : Features) {
    CurFeature = CurFeature.trim();
    if (CurFeature == "default")
      continue;
    if (!Context.getTargetInfo().validateCpuSupports(CurFeature))
      return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
             << Unsupported << None << CurFeature << TargetVersion;
  }
  return false;
}

static void handleTargetVersionAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  StringRef Str;
  SourceLocation LiteralLoc;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, Str, &LiteralLoc) ||
      S.checkTargetVersionAttr(LiteralLoc, D, Str))
    return;
  TargetVersionAttr *NewAttr =
      ::new (S.Context) TargetVersionAttr(S.Context, AL, Str);
  D->addAttr(NewAttr);
}

static void handleTargetAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  StringRef Str;
  SourceLocation LiteralLoc;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, Str, &LiteralLoc) ||
      S.checkTargetAttr(LiteralLoc, Str))
    return;

  TargetAttr *NewAttr = ::new (S.Context) TargetAttr(S.Context, AL, Str);
  D->addAttr(NewAttr);
}

bool Sema::checkTargetClonesAttrString(
    SourceLocation LiteralLoc, StringRef Str, const StringLiteral *Literal,
    Decl *D, bool &HasDefault, bool &HasCommas, bool &HasNotDefault,
    SmallVectorImpl<SmallString<64>> &StringsBuffer) {
  enum FirstParam { Unsupported, Duplicate, Unknown };
  enum SecondParam { None, CPU, Tune };
  enum ThirdParam { Target, TargetClones };
  HasCommas = HasCommas || Str.contains(',');
  const TargetInfo &TInfo = Context.getTargetInfo();
  // Warn on empty at the beginning of a string.
  if (Str.size() == 0)
    return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
           << Unsupported << None << "" << TargetClones;

  std::pair<StringRef, StringRef> Parts = {{}, Str};
  while (!Parts.second.empty()) {
    Parts = Parts.second.split(',');
    StringRef Cur = Parts.first.trim();
    SourceLocation CurLoc =
        Literal->getLocationOfByte(Cur.data() - Literal->getString().data(),
                                   getSourceManager(), getLangOpts(), TInfo);

    bool DefaultIsDupe = false;
    bool HasCodeGenImpact = false;
    if (Cur.empty())
      return Diag(CurLoc, diag::warn_unsupported_target_attribute)
             << Unsupported << None << "" << TargetClones;

    if (TInfo.getTriple().isAArch64()) {
      // AArch64 target clones specific
      if (Cur == "default") {
        DefaultIsDupe = HasDefault;
        HasDefault = true;
        if (llvm::is_contained(StringsBuffer, Cur) || DefaultIsDupe)
          Diag(CurLoc, diag::warn_target_clone_duplicate_options);
        else
          StringsBuffer.push_back(Cur);
      } else {
        std::pair<StringRef, StringRef> CurParts = {{}, Cur};
        llvm::SmallVector<StringRef, 8> CurFeatures;
        while (!CurParts.second.empty()) {
          CurParts = CurParts.second.split('+');
          StringRef CurFeature = CurParts.first.trim();
          if (!TInfo.validateCpuSupports(CurFeature)) {
            Diag(CurLoc, diag::warn_unsupported_target_attribute)
                << Unsupported << None << CurFeature << TargetClones;
            continue;
          }
          if (TInfo.doesFeatureAffectCodeGen(CurFeature))
            HasCodeGenImpact = true;
          CurFeatures.push_back(CurFeature);
        }
        // Canonize TargetClones Attributes
        llvm::sort(CurFeatures);
        SmallString<64> Res;
        for (auto &CurFeat : CurFeatures) {
          if (!Res.empty())
            Res.append("+");
          Res.append(CurFeat);
        }
        if (llvm::is_contained(StringsBuffer, Res) || DefaultIsDupe)
          Diag(CurLoc, diag::warn_target_clone_duplicate_options);
        else if (!HasCodeGenImpact)
          // Ignore features in target_clone attribute that don't impact
          // code generation
          Diag(CurLoc, diag::warn_target_clone_no_impact_options);
        else if (!Res.empty()) {
          StringsBuffer.push_back(Res);
          HasNotDefault = true;
        }
      }
    } else if (TInfo.getTriple().isRISCV()) {
      // Suppress warn_target_clone_mixed_values
      HasCommas = false;

      // Cur is split's parts of Str. RISC-V uses Str directly,
      // so skip when encountered more than once.
      if (!Str.starts_with(Cur))
        continue;

      llvm::SmallVector<StringRef, 8> AttrStrs;
      Str.split(AttrStrs, ";");

      bool IsPriority = false;
      bool IsDefault = false;
      for (auto &AttrStr : AttrStrs) {
        // Only support arch=+ext,... syntax.
        if (AttrStr.starts_with("arch=+")) {
          ParsedTargetAttr TargetAttr =
              Context.getTargetInfo().parseTargetAttr(AttrStr);

          if (TargetAttr.Features.empty() ||
              llvm::any_of(TargetAttr.Features, [&](const StringRef Ext) {
                return !RISCV().isValidFMVExtension(Ext);
              }))
            return Diag(CurLoc, diag::warn_unsupported_target_attribute)
                   << Unsupported << None << Str << TargetClones;
        } else if (AttrStr.starts_with("default")) {
          IsDefault = true;
          DefaultIsDupe = HasDefault;
          HasDefault = true;
        } else if (AttrStr.consume_front("priority=")) {
          IsPriority = true;
          unsigned Digit;
          if (AttrStr.getAsInteger(0, Digit))
            return Diag(CurLoc, diag::warn_unsupported_target_attribute)
                   << Unsupported << None << Str << TargetClones;
        } else {
          return Diag(CurLoc, diag::warn_unsupported_target_attribute)
                 << Unsupported << None << Str << TargetClones;
        }
      }

      if (IsPriority && IsDefault)
        return Diag(CurLoc, diag::warn_unsupported_target_attribute)
               << Unsupported << None << Str << TargetClones;

      if (llvm::is_contained(StringsBuffer, Str) || DefaultIsDupe)
        Diag(CurLoc, diag::warn_target_clone_duplicate_options);
      StringsBuffer.push_back(Str);
    } else {
      // Other targets ( currently X86 )
      if (Cur.starts_with("arch=")) {
        if (!Context.getTargetInfo().isValidCPUName(
                Cur.drop_front(sizeof("arch=") - 1)))
          return Diag(CurLoc, diag::warn_unsupported_target_attribute)
                 << Unsupported << CPU << Cur.drop_front(sizeof("arch=") - 1)
                 << TargetClones;
      } else if (Cur == "default") {
        DefaultIsDupe = HasDefault;
        HasDefault = true;
      } else if (!Context.getTargetInfo().isValidFeatureName(Cur) ||
                 Context.getTargetInfo().getFMVPriority(Cur) == 0)
        return Diag(CurLoc, diag::warn_unsupported_target_attribute)
               << Unsupported << None << Cur << TargetClones;
      if (llvm::is_contained(StringsBuffer, Cur) || DefaultIsDupe)
        Diag(CurLoc, diag::warn_target_clone_duplicate_options);
      // Note: Add even if there are duplicates, since it changes name mangling.
      StringsBuffer.push_back(Cur);
    }
  }
  if (Str.rtrim().ends_with(","))
    return Diag(LiteralLoc, diag::warn_unsupported_target_attribute)
           << Unsupported << None << "" << TargetClones;
  return false;
}

static void handleTargetClonesAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (S.Context.getTargetInfo().getTriple().isAArch64() &&
      !S.Context.getTargetInfo().hasFeature("fmv"))
    return;

  // Ensure we don't combine these with themselves, since that causes some
  // confusing behavior.
  if (const auto *Other = D->getAttr<TargetClonesAttr>()) {
    S.Diag(AL.getLoc(), diag::err_disallowed_duplicate_attribute) << AL;
    S.Diag(Other->getLocation(), diag::note_conflicting_attribute);
    return;
  }
  if (checkAttrMutualExclusion<TargetClonesAttr>(S, D, AL))
    return;

  SmallVector<StringRef, 2> Strings;
  SmallVector<SmallString<64>, 2> StringsBuffer;
  bool HasCommas = false, HasDefault = false, HasNotDefault = false;

  for (unsigned I = 0, E = AL.getNumArgs(); I != E; ++I) {
    StringRef CurStr;
    SourceLocation LiteralLoc;
    if (!S.checkStringLiteralArgumentAttr(AL, I, CurStr, &LiteralLoc) ||
        S.checkTargetClonesAttrString(
            LiteralLoc, CurStr,
            cast<StringLiteral>(AL.getArgAsExpr(I)->IgnoreParenCasts()), D,
            HasDefault, HasCommas, HasNotDefault, StringsBuffer))
      return;
  }
  for (auto &SmallStr : StringsBuffer)
    Strings.push_back(SmallStr.str());

  if (HasCommas && AL.getNumArgs() > 1)
    S.Diag(AL.getLoc(), diag::warn_target_clone_mixed_values);

  if (!HasDefault && !S.Context.getTargetInfo().getTriple().isAArch64()) {
    S.Diag(AL.getLoc(), diag::err_target_clone_must_have_default);
    return;
  }

  // FIXME: We could probably figure out how to get this to work for lambdas
  // someday.
  if (const auto *MD = dyn_cast<CXXMethodDecl>(D)) {
    if (MD->getParent()->isLambda()) {
      S.Diag(D->getLocation(), diag::err_multiversion_doesnt_support)
          << static_cast<unsigned>(MultiVersionKind::TargetClones)
          << /*Lambda*/ 9;
      return;
    }
  }

  // No multiversion if we have default version only.
  if (S.Context.getTargetInfo().getTriple().isAArch64() && !HasNotDefault)
    return;

  cast<FunctionDecl>(D)->setIsMultiVersion();
  TargetClonesAttr *NewAttr = ::new (S.Context)
      TargetClonesAttr(S.Context, AL, Strings.data(), Strings.size());
  D->addAttr(NewAttr);
}

static void handleMinVectorWidthAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  Expr *E = AL.getArgAsExpr(0);
  uint32_t VecWidth;
  if (!S.checkUInt32Argument(AL, E, VecWidth)) {
    AL.setInvalid();
    return;
  }

  MinVectorWidthAttr *Existing = D->getAttr<MinVectorWidthAttr>();
  if (Existing && Existing->getVectorWidth() != VecWidth) {
    S.Diag(AL.getLoc(), diag::warn_duplicate_attribute) << AL;
    return;
  }

  D->addAttr(::new (S.Context) MinVectorWidthAttr(S.Context, AL, VecWidth));
}

static void handleCleanupAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  Expr *E = AL.getArgAsExpr(0);
  SourceLocation Loc = E->getExprLoc();
  FunctionDecl *FD = nullptr;
  DeclarationNameInfo NI;

  // gcc only allows for simple identifiers. Since we support more than gcc, we
  // will warn the user.
  if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (DRE->hasQualifier())
      S.Diag(Loc, diag::warn_cleanup_ext);
    FD = dyn_cast<FunctionDecl>(DRE->getDecl());
    NI = DRE->getNameInfo();
    if (!FD) {
      S.Diag(Loc, diag::err_attribute_cleanup_arg_not_function) << 1
        << NI.getName();
      return;
    }
  } else if (auto *ULE = dyn_cast<UnresolvedLookupExpr>(E)) {
    if (ULE->hasExplicitTemplateArgs())
      S.Diag(Loc, diag::warn_cleanup_ext);
    FD = S.ResolveSingleFunctionTemplateSpecialization(ULE, true);
    NI = ULE->getNameInfo();
    if (!FD) {
      S.Diag(Loc, diag::err_attribute_cleanup_arg_not_function) << 2
        << NI.getName();
      if (ULE->getType() == S.Context.OverloadTy)
        S.NoteAllOverloadCandidates(ULE);
      return;
    }
  } else {
    S.Diag(Loc, diag::err_attribute_cleanup_arg_not_function) << 0;
    return;
  }

  if (FD->getNumParams() != 1) {
    S.Diag(Loc, diag::err_attribute_cleanup_func_must_take_one_arg)
      << NI.getName();
    return;
  }

  // We're currently more strict than GCC about what function types we accept.
  // If this ever proves to be a problem it should be easy to fix.
  QualType Ty = S.Context.getPointerType(cast<VarDecl>(D)->getType());
  QualType ParamTy = FD->getParamDecl(0)->getType();
  if (!S.IsAssignConvertCompatible(S.CheckAssignmentConstraints(
          FD->getParamDecl(0)->getLocation(), ParamTy, Ty))) {
    S.Diag(Loc, diag::err_attribute_cleanup_func_arg_incompatible_type)
      << NI.getName() << ParamTy << Ty;
    return;
  }
  VarDecl *VD = cast<VarDecl>(D);
  // Create a reference to the variable declaration. This is a fake/dummy
  // reference.
  DeclRefExpr *VariableReference = DeclRefExpr::Create(
      S.Context, NestedNameSpecifierLoc{}, FD->getLocation(), VD, false,
      DeclarationNameInfo{VD->getDeclName(), VD->getLocation()}, VD->getType(),
      VK_LValue);

  // Create a unary operator expression that represents taking the address of
  // the variable. This is a fake/dummy expression.
  Expr *AddressOfVariable = UnaryOperator::Create(
      S.Context, VariableReference, UnaryOperatorKind::UO_AddrOf,
      S.Context.getPointerType(VD->getType()), VK_PRValue, OK_Ordinary, Loc,
      +false, FPOptionsOverride{});

  // Create a function call expression. This is a fake/dummy call expression.
  CallExpr *FunctionCallExpression =
      CallExpr::Create(S.Context, E, ArrayRef{AddressOfVariable},
                       S.Context.VoidTy, VK_PRValue, Loc, FPOptionsOverride{});

  if (S.CheckFunctionCall(FD, FunctionCallExpression,
                          FD->getType()->getAs<FunctionProtoType>())) {
    return;
  }

  auto *attr = ::new (S.Context) CleanupAttr(S.Context, AL, FD);
  attr->setArgLoc(E->getExprLoc());
  D->addAttr(attr);
}

static void handleEnumExtensibilityAttr(Sema &S, Decl *D,
                                        const ParsedAttr &AL) {
  if (!AL.isArgIdent(0)) {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_n_type)
        << AL << 0 << AANT_ArgumentIdentifier;
    return;
  }

  EnumExtensibilityAttr::Kind ExtensibilityKind;
  IdentifierInfo *II = AL.getArgAsIdent(0)->getIdentifierInfo();
  if (!EnumExtensibilityAttr::ConvertStrToKind(II->getName(),
                                               ExtensibilityKind)) {
    S.Diag(AL.getLoc(), diag::warn_attribute_type_not_supported) << AL << II;
    return;
  }

  D->addAttr(::new (S.Context)
                 EnumExtensibilityAttr(S.Context, AL, ExtensibilityKind));
}

/// Handle __attribute__((format_arg((idx)))) attribute based on
/// http://gcc.gnu.org/onlinedocs/gcc/Function-Attributes.html
static void handleFormatArgAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  const Expr *IdxExpr = AL.getArgAsExpr(0);
  ParamIdx Idx;
  if (!S.checkFunctionOrMethodParameterIndex(D, AL, 1, IdxExpr, Idx))
    return;

  // Make sure the format string is really a string.
  QualType Ty = getFunctionOrMethodParamType(D, Idx.getASTIndex());

  bool NotNSStringTy = !S.ObjC().isNSStringType(Ty);
  if (NotNSStringTy && !S.ObjC().isCFStringType(Ty) &&
      (!Ty->isPointerType() ||
       !Ty->castAs<PointerType>()->getPointeeType()->isCharType())) {
    S.Diag(AL.getLoc(), diag::err_format_attribute_not)
        << IdxExpr->getSourceRange() << getFunctionOrMethodParamRange(D, 0);
    return;
  }
  Ty = getFunctionOrMethodResultType(D);
  // replace instancetype with the class type
  auto Instancetype = S.Context.getObjCInstanceTypeDecl()->getTypeForDecl();
  if (Ty->getAs<TypedefType>() == Instancetype)
    if (auto *OMD = dyn_cast<ObjCMethodDecl>(D))
      if (auto *Interface = OMD->getClassInterface())
        Ty = S.Context.getObjCObjectPointerType(
            QualType(Interface->getTypeForDecl(), 0));
  if (!S.ObjC().isNSStringType(Ty, /*AllowNSAttributedString=*/true) &&
      !S.ObjC().isCFStringType(Ty) &&
      (!Ty->isPointerType() ||
       !Ty->castAs<PointerType>()->getPointeeType()->isCharType())) {
    S.Diag(AL.getLoc(), diag::err_format_attribute_result_not)
        << (NotNSStringTy ? "string type" : "NSString")
        << IdxExpr->getSourceRange() << getFunctionOrMethodParamRange(D, 0);
    return;
  }

  D->addAttr(::new (S.Context) FormatArgAttr(S.Context, AL, Idx));
}

enum FormatAttrKind {
  CFStringFormat,
  NSStringFormat,
  StrftimeFormat,
  SupportedFormat,
  IgnoredFormat,
  InvalidFormat
};

/// getFormatAttrKind - Map from format attribute names to supported format
/// types.
static FormatAttrKind getFormatAttrKind(StringRef Format) {
  return llvm::StringSwitch<FormatAttrKind>(Format)
      // Check for formats that get handled specially.
      .Case("NSString", NSStringFormat)
      .Case("CFString", CFStringFormat)
      .Case("strftime", StrftimeFormat)

      // Otherwise, check for supported formats.
      .Cases("scanf", "printf", "printf0", "strfmon", SupportedFormat)
      .Cases("cmn_err", "vcmn_err", "zcmn_err", SupportedFormat)
      .Cases("kprintf", "syslog", SupportedFormat) // OpenBSD.
      .Case("freebsd_kprintf", SupportedFormat)    // FreeBSD.
      .Case("os_trace", SupportedFormat)
      .Case("os_log", SupportedFormat)

      .Cases("gcc_diag", "gcc_cdiag", "gcc_cxxdiag", "gcc_tdiag", IgnoredFormat)
      .Default(InvalidFormat);
}

/// Handle __attribute__((init_priority(priority))) attributes based on
/// http://gcc.gnu.org/onlinedocs/gcc/C_002b_002b-Attributes.html
static void handleInitPriorityAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!S.getLangOpts().CPlusPlus) {
    S.Diag(AL.getLoc(), diag::warn_attribute_ignored) << AL;
    return;
  }

  if (S.getLangOpts().HLSL) {
    S.Diag(AL.getLoc(), diag::err_hlsl_init_priority_unsupported);
    return;
  }

  if (S.getCurFunctionOrMethodDecl()) {
    S.Diag(AL.getLoc(), diag::err_init_priority_object_attr);
    AL.setInvalid();
    return;
  }
  QualType T = cast<VarDecl>(D)->getType();
  if (S.Context.getAsArrayType(T))
    T = S.Context.getBaseElementType(T);
  if (!T->getAs<RecordType>()) {
    S.Diag(AL.getLoc(), diag::err_init_priority_object_attr);
    AL.setInvalid();
    return;
  }

  Expr *E = AL.getArgAsExpr(0);
  uint32_t prioritynum;
  if (!S.checkUInt32Argument(AL, E, prioritynum)) {
    AL.setInvalid();
    return;
  }

  if (prioritynum > 65535) {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_out_of_range)
        << E->getSourceRange() << AL << 0 << 65535;
    AL.setInvalid();
    return;
  }

  // Values <= 100 are reserved for the implementation, and libc++
  // benefits from being able to specify values in that range.
  if (prioritynum < 101)
    S.Diag(AL.getLoc(), diag::warn_init_priority_reserved)
        << E->getSourceRange() << prioritynum;
  D->addAttr(::new (S.Context) InitPriorityAttr(S.Context, AL, prioritynum));
}

ErrorAttr *Sema::mergeErrorAttr(Decl *D, const AttributeCommonInfo &CI,
                                StringRef NewUserDiagnostic) {
  if (const auto *EA = D->getAttr<ErrorAttr>()) {
    std::string NewAttr = CI.getNormalizedFullName();
    assert((NewAttr == "error" || NewAttr == "warning") &&
           "unexpected normalized full name");
    bool Match = (EA->isError() && NewAttr == "error") ||
                 (EA->isWarning() && NewAttr == "warning");
    if (!Match) {
      Diag(EA->getLocation(), diag::err_attributes_are_not_compatible)
          << CI << EA
          << (CI.isRegularKeywordAttribute() ||
              EA->isRegularKeywordAttribute());
      Diag(CI.getLoc(), diag::note_conflicting_attribute);
      return nullptr;
    }
    if (EA->getUserDiagnostic() != NewUserDiagnostic) {
      Diag(CI.getLoc(), diag::warn_duplicate_attribute) << EA;
      Diag(EA->getLoc(), diag::note_previous_attribute);
    }
    D->dropAttr<ErrorAttr>();
  }
  return ::new (Context) ErrorAttr(Context, CI, NewUserDiagnostic);
}

FormatAttr *Sema::mergeFormatAttr(Decl *D, const AttributeCommonInfo &CI,
                                  IdentifierInfo *Format, int FormatIdx,
                                  int FirstArg) {
  // Check whether we already have an equivalent format attribute.
  for (auto *F : D->specific_attrs<FormatAttr>()) {
    if (F->getType() == Format &&
        F->getFormatIdx() == FormatIdx &&
        F->getFirstArg() == FirstArg) {
      // If we don't have a valid location for this attribute, adopt the
      // location.
      if (F->getLocation().isInvalid())
        F->setRange(CI.getRange());
      return nullptr;
    }
  }

  return ::new (Context) FormatAttr(Context, CI, Format, FormatIdx, FirstArg);
}

FormatMatchesAttr *Sema::mergeFormatMatchesAttr(Decl *D,
                                                const AttributeCommonInfo &CI,
                                                IdentifierInfo *Format,
                                                int FormatIdx,
                                                StringLiteral *FormatStr) {
  // Check whether we already have an equivalent FormatMatches attribute.
  for (auto *F : D->specific_attrs<FormatMatchesAttr>()) {
    if (F->getType() == Format && F->getFormatIdx() == FormatIdx) {
      if (!CheckFormatStringsCompatible(GetFormatStringType(Format->getName()),
                                        F->getFormatString(), FormatStr))
        return nullptr;

      // If we don't have a valid location for this attribute, adopt the
      // location.
      if (F->getLocation().isInvalid())
        F->setRange(CI.getRange());
      return nullptr;
    }
  }

  return ::new (Context)
      FormatMatchesAttr(Context, CI, Format, FormatIdx, FormatStr);
}

struct FormatAttrCommon {
  FormatAttrKind Kind;
  IdentifierInfo *Identifier;
  unsigned NumArgs;
  unsigned FormatStringIdx;
};

/// Handle __attribute__((format(type,idx,firstarg))) attributes based on
/// http://gcc.gnu.org/onlinedocs/gcc/Function-Attributes.html
static bool handleFormatAttrCommon(Sema &S, Decl *D, const ParsedAttr &AL,
                                   FormatAttrCommon *Info) {
  // Checks the first two arguments of the attribute; this is shared between
  // Format and FormatMatches attributes.

  if (!AL.isArgIdent(0)) {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_n_type)
        << AL << 1 << AANT_ArgumentIdentifier;
    return false;
  }

  // In C++ the implicit 'this' function parameter also counts, and they are
  // counted from one.
  bool HasImplicitThisParam = isInstanceMethod(D);
  Info->NumArgs = getFunctionOrMethodNumParams(D) + HasImplicitThisParam;

  Info->Identifier = AL.getArgAsIdent(0)->getIdentifierInfo();
  StringRef Format = Info->Identifier->getName();

  if (normalizeName(Format)) {
    // If we've modified the string name, we need a new identifier for it.
    Info->Identifier = &S.Context.Idents.get(Format);
  }

  // Check for supported formats.
  Info->Kind = getFormatAttrKind(Format);

  if (Info->Kind == IgnoredFormat)
    return false;

  if (Info->Kind == InvalidFormat) {
    S.Diag(AL.getLoc(), diag::warn_attribute_type_not_supported)
        << AL << Info->Identifier->getName();
    return false;
  }

  // checks for the 2nd argument
  Expr *IdxExpr = AL.getArgAsExpr(1);
  if (!S.checkUInt32Argument(AL, IdxExpr, Info->FormatStringIdx, 2))
    return false;

  if (Info->FormatStringIdx < 1 || Info->FormatStringIdx > Info->NumArgs) {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_out_of_bounds)
        << AL << 2 << IdxExpr->getSourceRange();
    return false;
  }

  // FIXME: Do we need to bounds check?
  unsigned ArgIdx = Info->FormatStringIdx - 1;

  if (HasImplicitThisParam) {
    if (ArgIdx == 0) {
      S.Diag(AL.getLoc(),
             diag::err_format_attribute_implicit_this_format_string)
          << IdxExpr->getSourceRange();
      return false;
    }
    ArgIdx--;
  }

  // make sure the format string is really a string
  QualType Ty = getFunctionOrMethodParamType(D, ArgIdx);

  if (!S.ObjC().isNSStringType(Ty, true) && !S.ObjC().isCFStringType(Ty) &&
      (!Ty->isPointerType() ||
       !Ty->castAs<PointerType>()->getPointeeType()->isCharType())) {
    S.Diag(AL.getLoc(), diag::err_format_attribute_not)
        << IdxExpr->getSourceRange()
        << getFunctionOrMethodParamRange(D, ArgIdx);
    return false;
  }

  return true;
}

static void handleFormatAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  FormatAttrCommon Info;
  if (!handleFormatAttrCommon(S, D, AL, &Info))
    return;

  // check the 3rd argument
  Expr *FirstArgExpr = AL.getArgAsExpr(2);
  uint32_t FirstArg;
  if (!S.checkUInt32Argument(AL, FirstArgExpr, FirstArg, 3))
    return;

  // FirstArg == 0 is is always valid.
  if (FirstArg != 0) {
    if (Info.Kind == StrftimeFormat) {
      // If the kind is strftime, FirstArg must be 0 because strftime does not
      // use any variadic arguments.
      S.Diag(AL.getLoc(), diag::err_format_strftime_third_parameter)
          << FirstArgExpr->getSourceRange()
          << FixItHint::CreateReplacement(FirstArgExpr->getSourceRange(), "0");
      return;
    } else if (isFunctionOrMethodVariadic(D)) {
      // Else, if the function is variadic, then FirstArg must be 0 or the
      // "position" of the ... parameter. It's unusual to use 0 with variadic
      // functions, so the fixit proposes the latter.
      if (FirstArg != Info.NumArgs + 1) {
        S.Diag(AL.getLoc(), diag::err_attribute_argument_out_of_bounds)
            << AL << 3 << FirstArgExpr->getSourceRange()
            << FixItHint::CreateReplacement(FirstArgExpr->getSourceRange(),
                                            std::to_string(Info.NumArgs + 1));
        return;
      }
    } else {
      // Inescapable GCC compatibility diagnostic.
      S.Diag(D->getLocation(), diag::warn_gcc_requires_variadic_function) << AL;
      if (FirstArg <= Info.FormatStringIdx) {
        // Else, the function is not variadic, and FirstArg must be 0 or any
        // parameter after the format parameter. We don't offer a fixit because
        // there are too many possible good values.
        S.Diag(AL.getLoc(), diag::err_attribute_argument_out_of_bounds)
            << AL << 3 << FirstArgExpr->getSourceRange();
        return;
      }
    }
  }

  FormatAttr *NewAttr =
      S.mergeFormatAttr(D, AL, Info.Identifier, Info.FormatStringIdx, FirstArg);
  if (NewAttr)
    D->addAttr(NewAttr);
}

static void handleFormatMatchesAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  FormatAttrCommon Info;
  if (!handleFormatAttrCommon(S, D, AL, &Info))
    return;

  Expr *FormatStrExpr = AL.getArgAsExpr(2)->IgnoreParenImpCasts();
  if (auto *SL = dyn_cast<StringLiteral>(FormatStrExpr)) {
    FormatStringType FST = S.GetFormatStringType(Info.Identifier->getName());
    if (S.ValidateFormatString(FST, SL))
      if (auto *NewAttr = S.mergeFormatMatchesAttr(D, AL, Info.Identifier,
                                                   Info.FormatStringIdx, SL))
        D->addAttr(NewAttr);
    return;
  }

  S.Diag(AL.getLoc(), diag::err_format_nonliteral)
      << FormatStrExpr->getSourceRange();
}

/// Handle __attribute__((callback(CalleeIdx, PayloadIdx0, ...))) attributes.
static void handleCallbackAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // The index that identifies the callback callee is mandatory.
  if (AL.getNumArgs() == 0) {
    S.Diag(AL.getLoc(), diag::err_callback_attribute_no_callee)
        << AL.getRange();
    return;
  }

  bool HasImplicitThisParam = isInstanceMethod(D);
  int32_t NumArgs = getFunctionOrMethodNumParams(D);

  FunctionDecl *FD = D->getAsFunction();
  assert(FD && "Expected a function declaration!");

  llvm::StringMap<int> NameIdxMapping;
  NameIdxMapping["__"] = -1;

  NameIdxMapping["this"] = 0;

  int Idx = 1;
  for (const ParmVarDecl *PVD : FD->parameters())
    NameIdxMapping[PVD->getName()] = Idx++;

  auto UnknownName = NameIdxMapping.end();

  SmallVector<int, 8> EncodingIndices;
  for (unsigned I = 0, E = AL.getNumArgs(); I < E; ++I) {
    SourceRange SR;
    int32_t ArgIdx;

    if (AL.isArgIdent(I)) {
      IdentifierLoc *IdLoc = AL.getArgAsIdent(I);
      auto It = NameIdxMapping.find(IdLoc->getIdentifierInfo()->getName());
      if (It == UnknownName) {
        S.Diag(AL.getLoc(), diag::err_callback_attribute_argument_unknown)
            << IdLoc->getIdentifierInfo() << IdLoc->getLoc();
        return;
      }

      SR = SourceRange(IdLoc->getLoc());
      ArgIdx = It->second;
    } else if (AL.isArgExpr(I)) {
      Expr *IdxExpr = AL.getArgAsExpr(I);

      // If the expression is not parseable as an int32_t we have a problem.
      if (!S.checkUInt32Argument(AL, IdxExpr, (uint32_t &)ArgIdx, I + 1,
                                 false)) {
        S.Diag(AL.getLoc(), diag::err_attribute_argument_out_of_bounds)
            << AL << (I + 1) << IdxExpr->getSourceRange();
        return;
      }

      // Check oob, excluding the special values, 0 and -1.
      if (ArgIdx < -1 || ArgIdx > NumArgs) {
        S.Diag(AL.getLoc(), diag::err_attribute_argument_out_of_bounds)
            << AL << (I + 1) << IdxExpr->getSourceRange();
        return;
      }

      SR = IdxExpr->getSourceRange();
    } else {
      llvm_unreachable("Unexpected ParsedAttr argument type!");
    }

    if (ArgIdx == 0 && !HasImplicitThisParam) {
      S.Diag(AL.getLoc(), diag::err_callback_implicit_this_not_available)
          << (I + 1) << SR;
      return;
    }

    // Adjust for the case we do not have an implicit "this" parameter. In this
    // case we decrease all positive values by 1 to get LLVM argument indices.
    if (!HasImplicitThisParam && ArgIdx > 0)
      ArgIdx -= 1;

    EncodingIndices.push_back(ArgIdx);
  }

  int CalleeIdx = EncodingIndices.front();
  // Check if the callee index is proper, thus not "this" and not "unknown".
  // This means the "CalleeIdx" has to be non-negative if "HasImplicitThisParam"
  // is false and positive if "HasImplicitThisParam" is true.
  if (CalleeIdx < (int)HasImplicitThisParam) {
    S.Diag(AL.getLoc(), diag::err_callback_attribute_invalid_callee)
        << AL.getRange();
    return;
  }

  // Get the callee type, note the index adjustment as the AST doesn't contain
  // the this type (which the callee cannot reference anyway!).
  const Type *CalleeType =
      getFunctionOrMethodParamType(D, CalleeIdx - HasImplicitThisParam)
          .getTypePtr();
  if (!CalleeType || !CalleeType->isFunctionPointerType()) {
    S.Diag(AL.getLoc(), diag::err_callback_callee_no_function_type)
        << AL.getRange();
    return;
  }

  const Type *CalleeFnType =
      CalleeType->getPointeeType()->getUnqualifiedDesugaredType();

  // TODO: Check the type of the callee arguments.

  const auto *CalleeFnProtoType = dyn_cast<FunctionProtoType>(CalleeFnType);
  if (!CalleeFnProtoType) {
    S.Diag(AL.getLoc(), diag::err_callback_callee_no_function_type)
        << AL.getRange();
    return;
  }

  if (CalleeFnProtoType->getNumParams() != EncodingIndices.size() - 1) {
    S.Diag(AL.getLoc(), diag::err_attribute_wrong_arg_count_for_func)
        << AL << QualType{CalleeFnProtoType, 0}
        << CalleeFnProtoType->getNumParams()
        << (unsigned)(EncodingIndices.size() - 1);
    return;
  }

  if (CalleeFnProtoType->isVariadic()) {
    S.Diag(AL.getLoc(), diag::err_callback_callee_is_variadic) << AL.getRange();
    return;
  }

  // Do not allow multiple callback attributes.
  if (D->hasAttr<CallbackAttr>()) {
    S.Diag(AL.getLoc(), diag::err_callback_attribute_multiple) << AL.getRange();
    return;
  }

  D->addAttr(::new (S.Context) CallbackAttr(
      S.Context, AL, EncodingIndices.data(), EncodingIndices.size()));
}

LifetimeCaptureByAttr *Sema::ParseLifetimeCaptureByAttr(const ParsedAttr &AL,
                                                        StringRef ParamName) {
  // Atleast one capture by is required.
  if (AL.getNumArgs() == 0) {
    Diag(AL.getLoc(), diag::err_capture_by_attribute_no_entity)
        << AL.getRange();
    return nullptr;
  }
  unsigned N = AL.getNumArgs();
  auto ParamIdents =
      MutableArrayRef<IdentifierInfo *>(new (Context) IdentifierInfo *[N], N);
  auto ParamLocs =
      MutableArrayRef<SourceLocation>(new (Context) SourceLocation[N], N);
  bool IsValid = true;
  for (unsigned I = 0; I < N; ++I) {
    if (AL.isArgExpr(I)) {
      Expr *E = AL.getArgAsExpr(I);
      Diag(E->getExprLoc(), diag::err_capture_by_attribute_argument_unknown)
          << E << E->getExprLoc();
      IsValid = false;
      continue;
    }
    assert(AL.isArgIdent(I));
    IdentifierLoc *IdLoc = AL.getArgAsIdent(I);
    if (IdLoc->getIdentifierInfo()->getName() == ParamName) {
      Diag(IdLoc->getLoc(), diag::err_capture_by_references_itself)
          << IdLoc->getLoc();
      IsValid = false;
      continue;
    }
    ParamIdents[I] = IdLoc->getIdentifierInfo();
    ParamLocs[I] = IdLoc->getLoc();
  }
  if (!IsValid)
    return nullptr;
  SmallVector<int> FakeParamIndices(N, LifetimeCaptureByAttr::Invalid);
  auto *CapturedBy =
      LifetimeCaptureByAttr::Create(Context, FakeParamIndices.data(), N, AL);
  CapturedBy->setArgs(ParamIdents, ParamLocs);
  return CapturedBy;
}

static void handleLifetimeCaptureByAttr(Sema &S, Decl *D,
                                        const ParsedAttr &AL) {
  // Do not allow multiple attributes.
  if (D->hasAttr<LifetimeCaptureByAttr>()) {
    S.Diag(AL.getLoc(), diag::err_capture_by_attribute_multiple)
        << AL.getRange();
    return;
  }
  auto *PVD = dyn_cast<ParmVarDecl>(D);
  assert(PVD);
  auto *CaptureByAttr = S.ParseLifetimeCaptureByAttr(AL, PVD->getName());
  if (CaptureByAttr)
    D->addAttr(CaptureByAttr);
}

void Sema::LazyProcessLifetimeCaptureByParams(FunctionDecl *FD) {
  bool HasImplicitThisParam = isInstanceMethod(FD);
  SmallVector<LifetimeCaptureByAttr *, 1> Attrs;
  for (ParmVarDecl *PVD : FD->parameters())
    if (auto *A = PVD->getAttr<LifetimeCaptureByAttr>())
      Attrs.push_back(A);
  if (HasImplicitThisParam) {
    TypeSourceInfo *TSI = FD->getTypeSourceInfo();
    if (!TSI)
      return;
    AttributedTypeLoc ATL;
    for (TypeLoc TL = TSI->getTypeLoc();
         (ATL = TL.getAsAdjusted<AttributedTypeLoc>());
         TL = ATL.getModifiedLoc()) {
      if (auto *A = ATL.getAttrAs<LifetimeCaptureByAttr>())
        Attrs.push_back(const_cast<LifetimeCaptureByAttr *>(A));
    }
  }
  if (Attrs.empty())
    return;
  llvm::StringMap<int> NameIdxMapping = {
      {"global", LifetimeCaptureByAttr::Global},
      {"unknown", LifetimeCaptureByAttr::Unknown}};
  int Idx = 0;
  if (HasImplicitThisParam) {
    NameIdxMapping["this"] = 0;
    Idx++;
  }
  for (const ParmVarDecl *PVD : FD->parameters())
    NameIdxMapping[PVD->getName()] = Idx++;
  auto DisallowReservedParams = [&](StringRef Reserved) {
    for (const ParmVarDecl *PVD : FD->parameters())
      if (PVD->getName() == Reserved)
        Diag(PVD->getLocation(), diag::err_capture_by_param_uses_reserved_name)
            << (PVD->getName() == "unknown");
  };
  for (auto *CapturedBy : Attrs) {
    const auto &Entities = CapturedBy->getArgIdents();
    for (size_t I = 0; I < Entities.size(); ++I) {
      StringRef Name = Entities[I]->getName();
      auto It = NameIdxMapping.find(Name);
      if (It == NameIdxMapping.end()) {
        auto Loc = CapturedBy->getArgLocs()[I];
        if (!HasImplicitThisParam && Name == "this")
          Diag(Loc, diag::err_capture_by_implicit_this_not_available) << Loc;
        else
          Diag(Loc, diag::err_capture_by_attribute_argument_unknown)
              << Entities[I] << Loc;
        continue;
      }
      if (Name == "unknown" || Name == "global")
        DisallowReservedParams(Name);
      CapturedBy->setParamIdx(I, It->second);
    }
  }
}

static bool isFunctionLike(const Type &T) {
  // Check for explicit function types.
  // 'called_once' is only supported in Objective-C and it has
  // function pointers and block pointers.
  return T.isFunctionPointerType() || T.isBlockPointerType();
}

/// Handle 'called_once' attribute.
static void handleCalledOnceAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // 'called_once' only applies to parameters representing functions.
  QualType T = cast<ParmVarDecl>(D)->getType();

  if (!isFunctionLike(*T)) {
    S.Diag(AL.getLoc(), diag::err_called_once_attribute_wrong_type);
    return;
  }

  D->addAttr(::new (S.Context) CalledOnceAttr(S.Context, AL));
}

static void handleTransparentUnionAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // Try to find the underlying union declaration.
  RecordDecl *RD = nullptr;
  const auto *TD = dyn_cast<TypedefNameDecl>(D);
  if (TD && TD->getUnderlyingType()->isUnionType())
    RD = TD->getUnderlyingType()->getAsUnionType()->getDecl();
  else
    RD = dyn_cast<RecordDecl>(D);

  if (!RD || !RD->isUnion()) {
    S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type)
        << AL << AL.isRegularKeywordAttribute() << ExpectedUnion;
    return;
  }

  if (!RD->isCompleteDefinition()) {
    if (!RD->isBeingDefined())
      S.Diag(AL.getLoc(),
             diag::warn_transparent_union_attribute_not_definition);
    return;
  }

  RecordDecl::field_iterator Field = RD->field_begin(),
                          FieldEnd = RD->field_end();
  if (Field == FieldEnd) {
    S.Diag(AL.getLoc(), diag::warn_transparent_union_attribute_zero_fields);
    return;
  }

  FieldDecl *FirstField = *Field;
  QualType FirstType = FirstField->getType();
  if (FirstType->hasFloatingRepresentation() || FirstType->isVectorType()) {
    S.Diag(FirstField->getLocation(),
           diag::warn_transparent_union_attribute_floating)
      << FirstType->isVectorType() << FirstType;
    return;
  }

  if (FirstType->isIncompleteType())
    return;
  uint64_t FirstSize = S.Context.getTypeSize(FirstType);
  uint64_t FirstAlign = S.Context.getTypeAlign(FirstType);
  for (; Field != FieldEnd; ++Field) {
    QualType FieldType = Field->getType();
    if (FieldType->isIncompleteType())
      return;
    // FIXME: this isn't fully correct; we also need to test whether the
    // members of the union would all have the same calling convention as the
    // first member of the union. Checking just the size and alignment isn't
    // sufficient (consider structs passed on the stack instead of in registers
    // as an example).
    if (S.Context.getTypeSize(FieldType) != FirstSize ||
        S.Context.getTypeAlign(FieldType) > FirstAlign) {
      // Warn if we drop the attribute.
      bool isSize = S.Context.getTypeSize(FieldType) != FirstSize;
      unsigned FieldBits = isSize ? S.Context.getTypeSize(FieldType)
                                  : S.Context.getTypeAlign(FieldType);
      S.Diag(Field->getLocation(),
             diag::warn_transparent_union_attribute_field_size_align)
          << isSize << *Field << FieldBits;
      unsigned FirstBits = isSize ? FirstSize : FirstAlign;
      S.Diag(FirstField->getLocation(),
             diag::note_transparent_union_first_field_size_align)
          << isSize << FirstBits;
      return;
    }
  }

  RD->addAttr(::new (S.Context) TransparentUnionAttr(S.Context, AL));
}

static void handleAnnotateAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  auto *Attr = S.CreateAnnotationAttr(AL);
  if (Attr) {
    D->addAttr(Attr);
  }
}

static void handleAlignValueAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  S.AddAlignValueAttr(D, AL, AL.getArgAsExpr(0));
}

void Sema::AddAlignValueAttr(Decl *D, const AttributeCommonInfo &CI, Expr *E) {
  SourceLocation AttrLoc = CI.getLoc();

  QualType T;
  if (const auto *TD = dyn_cast<TypedefNameDecl>(D))
    T = TD->getUnderlyingType();
  else if (const auto *VD = dyn_cast<ValueDecl>(D))
    T = VD->getType();
  else
    llvm_unreachable("Unknown decl type for align_value");

  if (!T->isDependentType() && !T->isAnyPointerType() &&
      !T->isReferenceType() && !T->isMemberPointerType()) {
    Diag(AttrLoc, diag::warn_attribute_pointer_or_reference_only)
        << CI << T << D->getSourceRange();
    return;
  }

  if (!E->isValueDependent()) {
    llvm::APSInt Alignment;
    ExprResult ICE = VerifyIntegerConstantExpression(
        E, &Alignment, diag::err_align_value_attribute_argument_not_int);
    if (ICE.isInvalid())
      return;

    if (!Alignment.isPowerOf2()) {
      Diag(AttrLoc, diag::err_alignment_not_power_of_two)
        << E->getSourceRange();
      return;
    }

    D->addAttr(::new (Context) AlignValueAttr(Context, CI, ICE.get()));
    return;
  }

  // Save dependent expressions in the AST to be instantiated.
  D->addAttr(::new (Context) AlignValueAttr(Context, CI, E));
}

static void handleAlignedAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (AL.hasParsedType()) {
    const ParsedType &TypeArg = AL.getTypeArg();
    TypeSourceInfo *TInfo;
    (void)S.GetTypeFromParser(
        ParsedType::getFromOpaquePtr(TypeArg.getAsOpaquePtr()), &TInfo);
    if (AL.isPackExpansion() &&
        !TInfo->getType()->containsUnexpandedParameterPack()) {
      S.Diag(AL.getEllipsisLoc(),
             diag::err_pack_expansion_without_parameter_packs);
      return;
    }

    if (!AL.isPackExpansion() &&
        S.DiagnoseUnexpandedParameterPack(TInfo->getTypeLoc().getBeginLoc(),
                                          TInfo, Sema::UPPC_Expression))
      return;

    S.AddAlignedAttr(D, AL, TInfo, AL.isPackExpansion());
    return;
  }

  // check the attribute arguments.
  if (AL.getNumArgs() > 1) {
    S.Diag(AL.getLoc(), diag::err_attribute_wrong_number_arguments) << AL << 1;
    return;
  }

  if (AL.getNumArgs() == 0) {
    D->addAttr(::new (S.Context) AlignedAttr(S.Context, AL, true, nullptr));
    return;
  }

  Expr *E = AL.getArgAsExpr(0);
  if (AL.isPackExpansion() && !E->containsUnexpandedParameterPack()) {
    S.Diag(AL.getEllipsisLoc(),
           diag::err_pack_expansion_without_parameter_packs);
    return;
  }

  if (!AL.isPackExpansion() && S.DiagnoseUnexpandedParameterPack(E))
    return;

  S.AddAlignedAttr(D, AL, E, AL.isPackExpansion());
}

/// Perform checking of type validity
///
/// C++11 [dcl.align]p1:
///   An alignment-specifier may be applied to a variable or to a class
///   data member, but it shall not be applied to a bit-field, a function
///   parameter, the formal parameter of a catch clause, or a variable
///   declared with the register storage class specifier. An
///   alignment-specifier may also be applied to the declaration of a class
///   or enumeration type.
/// CWG 2354:
///   CWG agreed to remove permission for alignas to be applied to
///   enumerations.
/// C11 6.7.5/2:
///   An alignment attribute shall not be specified in a declaration of
///   a typedef, or a bit-field, or a function, or a parameter, or an
///   object declared with the register storage-class specifier.
static bool validateAlignasAppliedType(Sema &S, Decl *D,
                                       const AlignedAttr &Attr,
                                       SourceLocation AttrLoc) {
  int DiagKind = -1;
  if (isa<ParmVarDecl>(D)) {
    DiagKind = 0;
  } else if (const auto *VD = dyn_cast<VarDecl>(D)) {
    if (VD->getStorageClass() == SC_Register)
      DiagKind = 1;
    if (VD->isExceptionVariable())
      DiagKind = 2;
  } else if (const auto *FD = dyn_cast<FieldDecl>(D)) {
    if (FD->isBitField())
      DiagKind = 3;
  } else if (const auto *ED = dyn_cast<EnumDecl>(D)) {
    if (ED->getLangOpts().CPlusPlus)
      DiagKind = 4;
  } else if (!isa<TagDecl>(D)) {
    return S.Diag(AttrLoc, diag::err_attribute_wrong_decl_type)
           << &Attr << Attr.isRegularKeywordAttribute()
           << (Attr.isC11() ? ExpectedVariableOrField
                            : ExpectedVariableFieldOrTag);
  }
  if (DiagKind != -1) {
    return S.Diag(AttrLoc, diag::err_alignas_attribute_wrong_decl_type)
           << &Attr << DiagKind;
  }
  return false;
}

void Sema::AddAlignedAttr(Decl *D, const AttributeCommonInfo &CI, Expr *E,
                          bool IsPackExpansion) {
  AlignedAttr TmpAttr(Context, CI, true, E);
  SourceLocation AttrLoc = CI.getLoc();

  // C++11 alignas(...) and C11 _Alignas(...) have additional requirements.
  if (TmpAttr.isAlignas() &&
      validateAlignasAppliedType(*this, D, TmpAttr, AttrLoc))
    return;

  if (E->isValueDependent()) {
    // We can't support a dependent alignment on a non-dependent type,
    // because we have no way to model that a type is "alignment-dependent"
    // but not dependent in any other way.
    if (const auto *TND = dyn_cast<TypedefNameDecl>(D)) {
      if (!TND->getUnderlyingType()->isDependentType()) {
        Diag(AttrLoc, diag::err_alignment_dependent_typedef_name)
            << E->getSourceRange();
        return;
      }
    }

    // Save dependent expressions in the AST to be instantiated.
    AlignedAttr *AA = ::new (Context) AlignedAttr(Context, CI, true, E);
    AA->setPackExpansion(IsPackExpansion);
    D->addAttr(AA);
    return;
  }

  // FIXME: Cache the number on the AL object?
  llvm::APSInt Alignment;
  ExprResult ICE = VerifyIntegerConstantExpression(
      E, &Alignment, diag::err_aligned_attribute_argument_not_int);
  if (ICE.isInvalid())
    return;

  uint64_t MaximumAlignment = Sema::MaximumAlignment;
  if (Context.getTargetInfo().getTriple().isOSBinFormatCOFF())
    MaximumAlignment = std::min(MaximumAlignment, uint64_t(8192));
  if (Alignment > MaximumAlignment) {
    Diag(AttrLoc, diag::err_attribute_aligned_too_great)
        << MaximumAlignment << E->getSourceRange();
    return;
  }

  uint64_t AlignVal = Alignment.getZExtValue();
  // C++11 [dcl.align]p2:
  //   -- if the constant expression evaluates to zero, the alignment
  //      specifier shall have no effect
  // C11 6.7.5p6:
  //   An alignment specification of zero has no effect.
  if (!(TmpAttr.isAlignas() && !Alignment)) {
    if (!llvm::isPowerOf2_64(AlignVal)) {
      Diag(AttrLoc, diag::err_alignment_not_power_of_two)
        << E->getSourceRange();
      return;
    }
  }

  const auto *VD = dyn_cast<VarDecl>(D);
  if (VD) {
    unsigned MaxTLSAlign =
        Context.toCharUnitsFromBits(Context.getTargetInfo().getMaxTLSAlign())
            .getQuantity();
    if (MaxTLSAlign && AlignVal > MaxTLSAlign &&
        VD->getTLSKind() != VarDecl::TLS_None) {
      Diag(VD->getLocation(), diag::err_tls_var_aligned_over_maximum)
          << (unsigned)AlignVal << VD << MaxTLSAlign;
      return;
    }
  }

  // On AIX, an aligned attribute can not decrease the alignment when applied
  // to a variable declaration with vector type.
  if (VD && Context.getTargetInfo().getTriple().isOSAIX()) {
    const Type *Ty = VD->getType().getTypePtr();
    if (Ty->isVectorType() && AlignVal < 16) {
      Diag(VD->getLocation(), diag::warn_aligned_attr_underaligned)
          << VD->getType() << 16;
      return;
    }
  }

  AlignedAttr *AA = ::new (Context) AlignedAttr(Context, CI, true, ICE.get());
  AA->setPackExpansion(IsPackExpansion);
  AA->setCachedAlignmentValue(
      static_cast<unsigned>(AlignVal * Context.getCharWidth()));
  D->addAttr(AA);
}

void Sema::AddAlignedAttr(Decl *D, const AttributeCommonInfo &CI,
                          TypeSourceInfo *TS, bool IsPackExpansion) {
  AlignedAttr TmpAttr(Context, CI, false, TS);
  SourceLocation AttrLoc = CI.getLoc();

  // C++11 alignas(...) and C11 _Alignas(...) have additional requirements.
  if (TmpAttr.isAlignas() &&
      validateAlignasAppliedType(*this, D, TmpAttr, AttrLoc))
    return;

  if (TS->getType()->isDependentType()) {
    // We can't support a dependent alignment on a non-dependent type,
    // because we have no way to model that a type is "type-dependent"
    // but not dependent in any other way.
    if (const auto *TND = dyn_cast<TypedefNameDecl>(D)) {
      if (!TND->getUnderlyingType()->isDependentType()) {
        Diag(AttrLoc, diag::err_alignment_dependent_typedef_name)
            << TS->getTypeLoc().getSourceRange();
        return;
      }
    }

    AlignedAttr *AA = ::new (Context) AlignedAttr(Context, CI, false, TS);
    AA->setPackExpansion(IsPackExpansion);
    D->addAttr(AA);
    return;
  }

  const auto *VD = dyn_cast<VarDecl>(D);
  unsigned AlignVal = TmpAttr.getAlignment(Context);
  // On AIX, an aligned attribute can not decrease the alignment when applied
  // to a variable declaration with vector type.
  if (VD && Context.getTargetInfo().getTriple().isOSAIX()) {
    const Type *Ty = VD->getType().getTypePtr();
    if (Ty->isVectorType() &&
        Context.toCharUnitsFromBits(AlignVal).getQuantity() < 16) {
      Diag(VD->getLocation(), diag::warn_aligned_attr_underaligned)
          << VD->getType() << 16;
      return;
    }
  }

  AlignedAttr *AA = ::new (Context) AlignedAttr(Context, CI, false, TS);
  AA->setPackExpansion(IsPackExpansion);
  AA->setCachedAlignmentValue(AlignVal);
  D->addAttr(AA);
}

void Sema::CheckAlignasUnderalignment(Decl *D) {
  assert(D->hasAttrs() && "no attributes on decl");

  QualType UnderlyingTy, DiagTy;
  if (const auto *VD = dyn_cast<ValueDecl>(D)) {
    UnderlyingTy = DiagTy = VD->getType();
  } else {
    UnderlyingTy = DiagTy = Context.getTagDeclType(cast<TagDecl>(D));
    if (const auto *ED = dyn_cast<EnumDecl>(D))
      UnderlyingTy = ED->getIntegerType();
  }
  if (DiagTy->isDependentType() || DiagTy->isIncompleteType())
    return;

  // C++11 [dcl.align]p5, C11 6.7.5/4:
  //   The combined effect of all alignment attributes in a declaration shall
  //   not specify an alignment that is less strict than the alignment that
  //   would otherwise be required for the entity being declared.
  AlignedAttr *AlignasAttr = nullptr;
  AlignedAttr *LastAlignedAttr = nullptr;
  unsigned Align = 0;
  for (auto *I : D->specific_attrs<AlignedAttr>()) {
    if (I->isAlignmentDependent())
      return;
    if (I->isAlignas())
      AlignasAttr = I;
    Align = std::max(Align, I->getAlignment(Context));
    LastAlignedAttr = I;
  }

  if (Align && DiagTy->isSizelessType()) {
    Diag(LastAlignedAttr->getLocation(), diag::err_attribute_sizeless_type)
        << LastAlignedAttr << DiagTy;
  } else if (AlignasAttr && Align) {
    CharUnits RequestedAlign = Context.toCharUnitsFromBits(Align);
    CharUnits NaturalAlign = Context.getTypeAlignInChars(UnderlyingTy);
    if (NaturalAlign > RequestedAlign)
      Diag(AlignasAttr->getLocation(), diag::err_alignas_underaligned)
        << DiagTy << (unsigned)NaturalAlign.getQuantity();
  }
}

bool Sema::checkMSInheritanceAttrOnDefinition(
    CXXRecordDecl *RD, SourceRange Range, bool BestCase,
    MSInheritanceModel ExplicitModel) {
  assert(RD->hasDefinition() && "RD has no definition!");

  // We may not have seen base specifiers or any virtual methods yet.  We will
  // have to wait until the record is defined to catch any mismatches.
  if (!RD->getDefinition()->isCompleteDefinition())
    return false;

  // The unspecified model never matches what a definition could need.
  if (ExplicitModel == MSInheritanceModel::Unspecified)
    return false;

  if (BestCase) {
    if (RD->calculateInheritanceModel() == ExplicitModel)
      return false;
  } else {
    if (RD->calculateInheritanceModel() <= ExplicitModel)
      return false;
  }

  Diag(Range.getBegin(), diag::err_mismatched_ms_inheritance)
      << 0 /*definition*/;
  Diag(RD->getDefinition()->getLocation(), diag::note_defined_here) << RD;
  return true;
}

/// parseModeAttrArg - Parses attribute mode string and returns parsed type
/// attribute.
static void parseModeAttrArg(Sema &S, StringRef Str, unsigned &DestWidth,
                             bool &IntegerMode, bool &ComplexMode,
                             FloatModeKind &ExplicitType) {
  IntegerMode = true;
  ComplexMode = false;
  ExplicitType = FloatModeKind::NoFloat;
  switch (Str.size()) {
  case 2:
    switch (Str[0]) {
    case 'Q':
      DestWidth = 8;
      break;
    case 'H':
      DestWidth = 16;
      break;
    case 'S':
      DestWidth = 32;
      break;
    case 'D':
      DestWidth = 64;
      break;
    case 'X':
      DestWidth = 96;
      break;
    case 'K': // KFmode - IEEE quad precision (__float128)
      ExplicitType = FloatModeKind::Float128;
      DestWidth = Str[1] == 'I' ? 0 : 128;
      break;
    case 'T':
      ExplicitType = FloatModeKind::LongDouble;
      DestWidth = 128;
      break;
    case 'I':
      ExplicitType = FloatModeKind::Ibm128;
      DestWidth = Str[1] == 'I' ? 0 : 128;
      break;
    }
    if (Str[1] == 'F') {
      IntegerMode = false;
    } else if (Str[1] == 'C') {
      IntegerMode = false;
      ComplexMode = true;
    } else if (Str[1] != 'I') {
      DestWidth = 0;
    }
    break;
  case 4:
    // FIXME: glibc uses 'word' to define register_t; this is narrower than a
    // pointer on PIC16 and other embedded platforms.
    if (Str == "word")
      DestWidth = S.Context.getTargetInfo().getRegisterWidth();
    else if (Str == "byte")
      DestWidth = S.Context.getTargetInfo().getCharWidth();
    break;
  case 7:
    if (Str == "pointer")
      DestWidth = S.Context.getTargetInfo().getPointerWidth(LangAS::Default);
    break;
  case 11:
    if (Str == "unwind_word")
      DestWidth = S.Context.getTargetInfo().getUnwindWordWidth();
    break;
  }
}

/// handleModeAttr - This attribute modifies the width of a decl with primitive
/// type.
///
/// Despite what would be logical, the mode attribute is a decl attribute, not a
/// type attribute: 'int ** __attribute((mode(HI))) *G;' tries to make 'G' be
/// HImode, not an intermediate pointer.
static void handleModeAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // This attribute isn't documented, but glibc uses it.  It changes
  // the width of an int or unsigned int to the specified size.
  if (!AL.isArgIdent(0)) {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_type)
        << AL << AANT_ArgumentIdentifier;
    return;
  }

  IdentifierInfo *Name = AL.getArgAsIdent(0)->getIdentifierInfo();

  S.AddModeAttr(D, AL, Name);
}

void Sema::AddModeAttr(Decl *D, const AttributeCommonInfo &CI,
                       IdentifierInfo *Name, bool InInstantiation) {
  StringRef Str = Name->getName();
  normalizeName(Str);
  SourceLocation AttrLoc = CI.getLoc();

  unsigned DestWidth = 0;
  bool IntegerMode = true;
  bool ComplexMode = false;
  FloatModeKind ExplicitType = FloatModeKind::NoFloat;
  llvm::APInt VectorSize(64, 0);
  if (Str.size() >= 4 && Str[0] == 'V') {
    // Minimal length of vector mode is 4: 'V' + NUMBER(>=1) + TYPE(>=2).
    size_t StrSize = Str.size();
    size_t VectorStringLength = 0;
    while ((VectorStringLength + 1) < StrSize &&
           isdigit(Str[VectorStringLength + 1]))
      ++VectorStringLength;
    if (VectorStringLength &&
        !Str.substr(1, VectorStringLength).getAsInteger(10, VectorSize) &&
        VectorSize.isPowerOf2()) {
      parseModeAttrArg(*this, Str.substr(VectorStringLength + 1), DestWidth,
                       IntegerMode, ComplexMode, ExplicitType);
      // Avoid duplicate warning from template instantiation.
      if (!InInstantiation)
        Diag(AttrLoc, diag::warn_vector_mode_deprecated);
    } else {
      VectorSize = 0;
    }
  }

  if (!VectorSize)
    parseModeAttrArg(*this, Str, DestWidth, IntegerMode, ComplexMode,
                     ExplicitType);

  // FIXME: Sync this with InitializePredefinedMacros; we need to match int8_t
  // and friends, at least with glibc.
  // FIXME: Make sure floating-point mappings are accurate
  // FIXME: Support XF and TF types
  if (!DestWidth) {
    Diag(AttrLoc, diag::err_machine_mode) << 0 /*Unknown*/ << Name;
    return;
  }

  QualType OldTy;
  if (const auto *TD = dyn_cast<TypedefNameDecl>(D))
    OldTy = TD->getUnderlyingType();
  else if (const auto *ED = dyn_cast<EnumDecl>(D)) {
    // Something like 'typedef enum { X } __attribute__((mode(XX))) T;'.
    // Try to get type from enum declaration, default to int.
    OldTy = ED->getIntegerType();
    if (OldTy.isNull())
      OldTy = Context.IntTy;
  } else
    OldTy = cast<ValueDecl>(D)->getType();

  if (OldTy->isDependentType()) {
    D->addAttr(::new (Context) ModeAttr(Context, CI, Name));
    return;
  }

  // Base type can also be a vector type (see PR17453).
  // Distinguish between base type and base element type.
  QualType OldElemTy = OldTy;
  if (const auto *VT = OldTy->getAs<VectorType>())
    OldElemTy = VT->getElementType();

  // GCC allows 'mode' attribute on enumeration types (even incomplete), except
  // for vector modes. So, 'enum X __attribute__((mode(QI)));' forms a complete
  // type, 'enum { A } __attribute__((mode(V4SI)))' is rejected.
  if ((isa<EnumDecl>(D) || OldElemTy->getAs<EnumType>()) &&
      VectorSize.getBoolValue()) {
    Diag(AttrLoc, diag::err_enum_mode_vector_type) << Name << CI.getRange();
    return;
  }
  bool IntegralOrAnyEnumType = (OldElemTy->isIntegralOrEnumerationType() &&
                                !OldElemTy->isBitIntType()) ||
                               OldElemTy->getAs<EnumType>();

  if (!OldElemTy->getAs<BuiltinType>() && !OldElemTy->isComplexType() &&
      !IntegralOrAnyEnumType)
    Diag(AttrLoc, diag::err_mode_not_primitive);
  else if (IntegerMode) {
    if (!IntegralOrAnyEnumType)
      Diag(AttrLoc, diag::err_mode_wrong_type);
  } else if (ComplexMode) {
    if (!OldElemTy->isComplexType())
      Diag(AttrLoc, diag::err_mode_wrong_type);
  } else {
    if (!OldElemTy->isFloatingType())
      Diag(AttrLoc, diag::err_mode_wrong_type);
  }

  QualType NewElemTy;

  if (IntegerMode)
    NewElemTy = Context.getIntTypeForBitwidth(DestWidth,
                                              OldElemTy->isSignedIntegerType());
  else
    NewElemTy = Context.getRealTypeForBitwidth(DestWidth, ExplicitType);

  if (NewElemTy.isNull()) {
    // Only emit diagnostic on host for 128-bit mode attribute
    if (!(DestWidth == 128 &&
          (getLangOpts().CUDAIsDevice || getLangOpts().SYCLIsDevice)))
      Diag(AttrLoc, diag::err_machine_mode) << 1 /*Unsupported*/ << Name;
    return;
  }

  if (ComplexMode) {
    NewElemTy = Context.getComplexType(NewElemTy);
  }

  QualType NewTy = NewElemTy;
  if (VectorSize.getBoolValue()) {
    NewTy = Context.getVectorType(NewTy, VectorSize.getZExtValue(),
                                  VectorKind::Generic);
  } else if (const auto *OldVT = OldTy->getAs<VectorType>()) {
    // Complex machine mode does not support base vector types.
    if (ComplexMode) {
      Diag(AttrLoc, diag::err_complex_mode_vector_type);
      return;
    }
    unsigned NumElements = Context.getTypeSize(OldElemTy) *
                           OldVT->getNumElements() /
                           Context.getTypeSize(NewElemTy);
    NewTy =
        Context.getVectorType(NewElemTy, NumElements, OldVT->getVectorKind());
  }

  if (NewTy.isNull()) {
    Diag(AttrLoc, diag::err_mode_wrong_type);
    return;
  }

  // Install the new type.
  if (auto *TD = dyn_cast<TypedefNameDecl>(D))
    TD->setModedTypeSourceInfo(TD->getTypeSourceInfo(), NewTy);
  else if (auto *ED = dyn_cast<EnumDecl>(D))
    ED->setIntegerType(NewTy);
  else
    cast<ValueDecl>(D)->setType(NewTy);

  D->addAttr(::new (Context) ModeAttr(Context, CI, Name));
}

static void handleNonStringAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // This only applies to fields and variable declarations which have an array
  // type.
  QualType QT = cast<ValueDecl>(D)->getType();
  if (!QT->isArrayType() ||
      !QT->getBaseElementTypeUnsafe()->isAnyCharacterType()) {
    S.Diag(D->getBeginLoc(), diag::warn_attribute_non_character_array)
        << AL << AL.isRegularKeywordAttribute() << QT << AL.getRange();
    return;
  }

  D->addAttr(::new (S.Context) NonStringAttr(S.Context, AL));
}

static void handleNoDebugAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  D->addAttr(::new (S.Context) NoDebugAttr(S.Context, AL));
}

AlwaysInlineAttr *Sema::mergeAlwaysInlineAttr(Decl *D,
                                              const AttributeCommonInfo &CI,
                                              const IdentifierInfo *Ident) {
  if (OptimizeNoneAttr *Optnone = D->getAttr<OptimizeNoneAttr>()) {
    Diag(CI.getLoc(), diag::warn_attribute_ignored) << Ident;
    Diag(Optnone->getLocation(), diag::note_conflicting_attribute);
    return nullptr;
  }

  if (D->hasAttr<AlwaysInlineAttr>())
    return nullptr;

  return ::new (Context) AlwaysInlineAttr(Context, CI);
}

InternalLinkageAttr *Sema::mergeInternalLinkageAttr(Decl *D,
                                                    const ParsedAttr &AL) {
  if (const auto *VD = dyn_cast<VarDecl>(D)) {
    // Attribute applies to Var but not any subclass of it (like ParmVar,
    // ImplicitParm or VarTemplateSpecialization).
    if (VD->getKind() != Decl::Var) {
      Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type)
          << AL << AL.isRegularKeywordAttribute()
          << (getLangOpts().CPlusPlus ? ExpectedFunctionVariableOrClass
                                      : ExpectedVariableOrFunction);
      return nullptr;
    }
    // Attribute does not apply to non-static local variables.
    if (VD->hasLocalStorage()) {
      Diag(VD->getLocation(), diag::warn_internal_linkage_local_storage);
      return nullptr;
    }
  }

  return ::new (Context) InternalLinkageAttr(Context, AL);
}
InternalLinkageAttr *
Sema::mergeInternalLinkageAttr(Decl *D, const InternalLinkageAttr &AL) {
  if (const auto *VD = dyn_cast<VarDecl>(D)) {
    // Attribute applies to Var but not any subclass of it (like ParmVar,
    // ImplicitParm or VarTemplateSpecialization).
    if (VD->getKind() != Decl::Var) {
      Diag(AL.getLocation(), diag::warn_attribute_wrong_decl_type)
          << &AL << AL.isRegularKeywordAttribute()
          << (getLangOpts().CPlusPlus ? ExpectedFunctionVariableOrClass
                                      : ExpectedVariableOrFunction);
      return nullptr;
    }
    // Attribute does not apply to non-static local variables.
    if (VD->hasLocalStorage()) {
      Diag(VD->getLocation(), diag::warn_internal_linkage_local_storage);
      return nullptr;
    }
  }

  return ::new (Context) InternalLinkageAttr(Context, AL);
}

MinSizeAttr *Sema::mergeMinSizeAttr(Decl *D, const AttributeCommonInfo &CI) {
  if (OptimizeNoneAttr *Optnone = D->getAttr<OptimizeNoneAttr>()) {
    Diag(CI.getLoc(), diag::warn_attribute_ignored) << "'minsize'";
    Diag(Optnone->getLocation(), diag::note_conflicting_attribute);
    return nullptr;
  }

  if (D->hasAttr<MinSizeAttr>())
    return nullptr;

  return ::new (Context) MinSizeAttr(Context, CI);
}

OptimizeNoneAttr *Sema::mergeOptimizeNoneAttr(Decl *D,
                                              const AttributeCommonInfo &CI) {
  if (AlwaysInlineAttr *Inline = D->getAttr<AlwaysInlineAttr>()) {
    Diag(Inline->getLocation(), diag::warn_attribute_ignored) << Inline;
    Diag(CI.getLoc(), diag::note_conflicting_attribute);
    D->dropAttr<AlwaysInlineAttr>();
  }
  if (MinSizeAttr *MinSize = D->getAttr<MinSizeAttr>()) {
    Diag(MinSize->getLocation(), diag::warn_attribute_ignored) << MinSize;
    Diag(CI.getLoc(), diag::note_conflicting_attribute);
    D->dropAttr<MinSizeAttr>();
  }

  if (D->hasAttr<OptimizeNoneAttr>())
    return nullptr;

  return ::new (Context) OptimizeNoneAttr(Context, CI);
}

static void handleAlwaysInlineAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (AlwaysInlineAttr *Inline =
          S.mergeAlwaysInlineAttr(D, AL, AL.getAttrName()))
    D->addAttr(Inline);
}

static void handleMinSizeAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (MinSizeAttr *MinSize = S.mergeMinSizeAttr(D, AL))
    D->addAttr(MinSize);
}

static void handleOptimizeNoneAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (OptimizeNoneAttr *Optnone = S.mergeOptimizeNoneAttr(D, AL))
    D->addAttr(Optnone);
}

static void handleConstantAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  const auto *VD = cast<VarDecl>(D);
  if (VD->hasLocalStorage()) {
    S.Diag(AL.getLoc(), diag::err_cuda_nonstatic_constdev);
    return;
  }
  // constexpr variable may already get an implicit constant attr, which should
  // be replaced by the explicit constant attr.
  if (auto *A = D->getAttr<CUDAConstantAttr>()) {
    if (!A->isImplicit())
      return;
    D->dropAttr<CUDAConstantAttr>();
  }
  D->addAttr(::new (S.Context) CUDAConstantAttr(S.Context, AL));
}

static void handleSharedAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  const auto *VD = cast<VarDecl>(D);
  // extern __shared__ is only allowed on arrays with no length (e.g.
  // "int x[]").
  if (!S.getLangOpts().GPURelocatableDeviceCode && VD->hasExternalStorage() &&
      !isa<IncompleteArrayType>(VD->getType())) {
    S.Diag(AL.getLoc(), diag::err_cuda_extern_shared) << VD;
    return;
  }
  if (S.getLangOpts().CUDA && VD->hasLocalStorage() &&
      S.CUDA().DiagIfHostCode(AL.getLoc(), diag::err_cuda_host_shared)
          << S.CUDA().CurrentTarget())
    return;
  D->addAttr(::new (S.Context) CUDASharedAttr(S.Context, AL));
}

static void handleGlobalAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  const auto *FD = cast<FunctionDecl>(D);
  if (!FD->getReturnType()->isVoidType() &&
      !FD->getReturnType()->getAs<AutoType>() &&
      !FD->getReturnType()->isInstantiationDependentType()) {
    SourceRange RTRange = FD->getReturnTypeSourceRange();
    S.Diag(FD->getTypeSpecStartLoc(), diag::err_kern_type_not_void_return)
        << FD->getType()
        << (RTRange.isValid() ? FixItHint::CreateReplacement(RTRange, "void")
                              : FixItHint());
    return;
  }
  if (const auto *Method = dyn_cast<CXXMethodDecl>(FD)) {
    if (Method->isInstance()) {
      S.Diag(Method->getBeginLoc(), diag::err_kern_is_nonstatic_method)
          << Method;
      return;
    }
    S.Diag(Method->getBeginLoc(), diag::warn_kern_is_method) << Method;
  }
  // Only warn for "inline" when compiling for host, to cut down on noise.
  if (FD->isInlineSpecified() && !S.getLangOpts().CUDAIsDevice)
    S.Diag(FD->getBeginLoc(), diag::warn_kern_is_inline) << FD;

  if (AL.getKind() == ParsedAttr::AT_DeviceKernel)
    D->addAttr(::new (S.Context) DeviceKernelAttr(S.Context, AL));
  else
    D->addAttr(::new (S.Context) CUDAGlobalAttr(S.Context, AL));
  // In host compilation the kernel is emitted as a stub function, which is
  // a helper function for launching the kernel. The instructions in the helper
  // function has nothing to do with the source code of the kernel. Do not emit
  // debug info for the stub function to avoid confusing the debugger.
  if (S.LangOpts.HIP && !S.LangOpts.CUDAIsDevice)
    D->addAttr(NoDebugAttr::CreateImplicit(S.Context));
}

static void handleDeviceAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (const auto *VD = dyn_cast<VarDecl>(D)) {
    if (VD->hasLocalStorage()) {
      S.Diag(AL.getLoc(), diag::err_cuda_nonstatic_constdev);
      return;
    }
  }

  if (auto *A = D->getAttr<CUDADeviceAttr>()) {
    if (!A->isImplicit())
      return;
    D->dropAttr<CUDADeviceAttr>();
  }
  D->addAttr(::new (S.Context) CUDADeviceAttr(S.Context, AL));
}

static void handleManagedAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (const auto *VD = dyn_cast<VarDecl>(D)) {
    if (VD->hasLocalStorage()) {
      S.Diag(AL.getLoc(), diag::err_cuda_nonstatic_constdev);
      return;
    }
  }
  if (!D->hasAttr<HIPManagedAttr>())
    D->addAttr(::new (S.Context) HIPManagedAttr(S.Context, AL));
  if (!D->hasAttr<CUDADeviceAttr>())
    D->addAttr(CUDADeviceAttr::CreateImplicit(S.Context));
}

static void handleGridConstantAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (D->isInvalidDecl())
    return;
  // Whether __grid_constant__ is allowed to be used will be checked in
  // Sema::CheckFunctionDeclaration as we need complete function decl to make
  // the call.
  D->addAttr(::new (S.Context) CUDAGridConstantAttr(S.Context, AL));
}

static void handleGNUInlineAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  const auto *Fn = cast<FunctionDecl>(D);
  if (!Fn->isInlineSpecified()) {
    S.Diag(AL.getLoc(), diag::warn_gnu_inline_attribute_requires_inline);
    return;
  }

  if (S.LangOpts.CPlusPlus && Fn->getStorageClass() != SC_Extern)
    S.Diag(AL.getLoc(), diag::warn_gnu_inline_cplusplus_without_extern);

  D->addAttr(::new (S.Context) GNUInlineAttr(S.Context, AL));
}

static void handleCallConvAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (hasDeclarator(D)) return;

  // Diagnostic is emitted elsewhere: here we store the (valid) AL
  // in the Decl node for syntactic reasoning, e.g., pretty-printing.
  CallingConv CC;
  if (S.CheckCallingConvAttr(
          AL, CC, /*FD*/ nullptr,
          S.CUDA().IdentifyTarget(dyn_cast<FunctionDecl>(D))))
    return;

  if (!isa<ObjCMethodDecl>(D)) {
    S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type)
        << AL << AL.isRegularKeywordAttribute() << ExpectedFunctionOrMethod;
    return;
  }

  switch (AL.getKind()) {
  case ParsedAttr::AT_FastCall:
    D->addAttr(::new (S.Context) FastCallAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_StdCall:
    D->addAttr(::new (S.Context) StdCallAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_ThisCall:
    D->addAttr(::new (S.Context) ThisCallAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_CDecl:
    D->addAttr(::new (S.Context) CDeclAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_Pascal:
    D->addAttr(::new (S.Context) PascalAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_SwiftCall:
    D->addAttr(::new (S.Context) SwiftCallAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_SwiftAsyncCall:
    D->addAttr(::new (S.Context) SwiftAsyncCallAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_VectorCall:
    D->addAttr(::new (S.Context) VectorCallAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_MSABI:
    D->addAttr(::new (S.Context) MSABIAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_SysVABI:
    D->addAttr(::new (S.Context) SysVABIAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_RegCall:
    D->addAttr(::new (S.Context) RegCallAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_Pcs: {
    PcsAttr::PCSType PCS;
    switch (CC) {
    case CC_AAPCS:
      PCS = PcsAttr::AAPCS;
      break;
    case CC_AAPCS_VFP:
      PCS = PcsAttr::AAPCS_VFP;
      break;
    default:
      llvm_unreachable("unexpected calling convention in pcs attribute");
    }

    D->addAttr(::new (S.Context) PcsAttr(S.Context, AL, PCS));
    return;
  }
  case ParsedAttr::AT_AArch64VectorPcs:
    D->addAttr(::new (S.Context) AArch64VectorPcsAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_AArch64SVEPcs:
    D->addAttr(::new (S.Context) AArch64SVEPcsAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_DeviceKernel: {
    // The attribute should already be applied.
    assert(D->hasAttr<DeviceKernelAttr>() && "Expected attribute");
    return;
  }
  case ParsedAttr::AT_IntelOclBicc:
    D->addAttr(::new (S.Context) IntelOclBiccAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_PreserveMost:
    D->addAttr(::new (S.Context) PreserveMostAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_PreserveAll:
    D->addAttr(::new (S.Context) PreserveAllAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_M68kRTD:
    D->addAttr(::new (S.Context) M68kRTDAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_PreserveNone:
    D->addAttr(::new (S.Context) PreserveNoneAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_RISCVVectorCC:
    D->addAttr(::new (S.Context) RISCVVectorCCAttr(S.Context, AL));
    return;
  case ParsedAttr::AT_RISCVVLSCC: {
    // If the riscv_abi_vlen doesn't have any argument, default ABI_VLEN is 128.
    unsigned VectorLength = 128;
    if (AL.getNumArgs() &&
        !S.checkUInt32Argument(AL, AL.getArgAsExpr(0), VectorLength))
      return;
    if (VectorLength < 32 || VectorLength > 65536) {
      S.Diag(AL.getLoc(), diag::err_argument_invalid_range)
          << VectorLength << 32 << 65536;
      return;
    }
    if (!llvm::isPowerOf2_64(VectorLength)) {
      S.Diag(AL.getLoc(), diag::err_argument_not_power_of_2);
      return;
    }

    D->addAttr(::new (S.Context) RISCVVLSCCAttr(S.Context, AL, VectorLength));
    return;
  }
  default:
    llvm_unreachable("unexpected attribute kind");
  }
}

static void handleDeviceKernelAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  const auto *FD = dyn_cast_or_null<FunctionDecl>(D);
  bool IsFunctionTemplate = FD && FD->getDescribedFunctionTemplate();
  if (S.getLangOpts().SYCLIsDevice) {
    if (!IsFunctionTemplate) {
      S.Diag(AL.getLoc(), diag::warn_attribute_wrong_decl_type_str)
          << AL << AL.isRegularKeywordAttribute() << "function templates";
    } else {
      S.SYCL().handleKernelAttr(D, AL);
    }
  } else if (DeviceKernelAttr::isSYCLSpelling(AL)) {
    S.Diag(AL.getLoc(), diag::warn_attribute_ignored) << AL;
  } else if (S.getASTContext().getTargetInfo().getTriple().isNVPTX()) {
    handleGlobalAttr(S, D, AL);
  } else {
    // OpenCL C++ will throw a more specific error.
    if (!S.getLangOpts().OpenCLCPlusPlus && (!FD || IsFunctionTemplate)) {
      S.Diag(AL.getLoc(), diag::err_attribute_wrong_decl_type_str)
          << AL << AL.isRegularKeywordAttribute() << "functions";
    }
    handleSimpleAttribute<DeviceKernelAttr>(S, D, AL);
  }
  // Make sure we validate the CC with the target
  // and warn/error if necessary.
  handleCallConvAttr(S, D, AL);
}

static void handleSuppressAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (AL.getAttributeSpellingListIndex() == SuppressAttr::CXX11_gsl_suppress) {
    // Suppression attribute with GSL spelling requires at least 1 argument.
    if (!AL.checkAtLeastNumArgs(S, 1))
      return;
  }

  std::vector<StringRef> DiagnosticIdentifiers;
  for (unsigned I = 0, E = AL.getNumArgs(); I != E; ++I) {
    StringRef RuleName;

    if (!S.checkStringLiteralArgumentAttr(AL, I, RuleName, nullptr))
      return;

    DiagnosticIdentifiers.push_back(RuleName);
  }
  D->addAttr(::new (S.Context)
                 SuppressAttr(S.Context, AL, DiagnosticIdentifiers.data(),
                              DiagnosticIdentifiers.size()));
}

static void handleLifetimeCategoryAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  TypeSourceInfo *DerefTypeLoc = nullptr;
  QualType ParmType;
  if (AL.hasParsedType()) {
    ParmType = S.GetTypeFromParser(AL.getTypeArg(), &DerefTypeLoc);

    unsigned SelectIdx = ~0U;
    if (ParmType->isReferenceType())
      SelectIdx = 0;
    else if (ParmType->isArrayType())
      SelectIdx = 1;

    if (SelectIdx != ~0U) {
      S.Diag(AL.getLoc(), diag::err_attribute_invalid_argument)
          << SelectIdx << AL;
      return;
    }
  }

  // To check if earlier decl attributes do not conflict the newly parsed ones
  // we always add (and check) the attribute to the canonical decl. We need
  // to repeat the check for attribute mutual exclusion because we're attaching
  // all of the attributes to the canonical declaration rather than the current
  // declaration.
  D = D->getCanonicalDecl();
  if (AL.getKind() == ParsedAttr::AT_Owner) {
    if (checkAttrMutualExclusion<PointerAttr>(S, D, AL))
      return;
    if (const auto *OAttr = D->getAttr<OwnerAttr>()) {
      const Type *ExistingDerefType = OAttr->getDerefTypeLoc()
                                          ? OAttr->getDerefType().getTypePtr()
                                          : nullptr;
      if (ExistingDerefType != ParmType.getTypePtrOrNull()) {
        S.Diag(AL.getLoc(), diag::err_attributes_are_not_compatible)
            << AL << OAttr
            << (AL.isRegularKeywordAttribute() ||
                OAttr->isRegularKeywordAttribute());
        S.Diag(OAttr->getLocation(), diag::note_conflicting_attribute);
      }
      return;
    }
    for (Decl *Redecl : D->redecls()) {
      Redecl->addAttr(::new (S.Context) OwnerAttr(S.Context, AL, DerefTypeLoc));
    }
  } else {
    if (checkAttrMutualExclusion<OwnerAttr>(S, D, AL))
      return;
    if (const auto *PAttr = D->getAttr<PointerAttr>()) {
      const Type *ExistingDerefType = PAttr->getDerefTypeLoc()
                                          ? PAttr->getDerefType().getTypePtr()
                                          : nullptr;
      if (ExistingDerefType != ParmType.getTypePtrOrNull()) {
        S.Diag(AL.getLoc(), diag::err_attributes_are_not_compatible)
            << AL << PAttr
            << (AL.isRegularKeywordAttribute() ||
                PAttr->isRegularKeywordAttribute());
        S.Diag(PAttr->getLocation(), diag::note_conflicting_attribute);
      }
      return;
    }
    for (Decl *Redecl : D->redecls()) {
      Redecl->addAttr(::new (S.Context)
                          PointerAttr(S.Context, AL, DerefTypeLoc));
    }
  }
}

static void handleRandomizeLayoutAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (checkAttrMutualExclusion<NoRandomizeLayoutAttr>(S, D, AL))
    return;
  if (!D->hasAttr<RandomizeLayoutAttr>())
    D->addAttr(::new (S.Context) RandomizeLayoutAttr(S.Context, AL));
}

static void handleNoRandomizeLayoutAttr(Sema &S, Decl *D,
                                        const ParsedAttr &AL) {
  if (checkAttrMutualExclusion<RandomizeLayoutAttr>(S, D, AL))
    return;
  if (!D->hasAttr<NoRandomizeLayoutAttr>())
    D->addAttr(::new (S.Context) NoRandomizeLayoutAttr(S.Context, AL));
}

bool Sema::CheckCallingConvAttr(const ParsedAttr &Attrs, CallingConv &CC,
                                const FunctionDecl *FD,
                                CUDAFunctionTarget CFT) {
  if (Attrs.isInvalid())
    return true;

  if (Attrs.hasProcessingCache()) {
    CC = (CallingConv) Attrs.getProcessingCache();
    return false;
  }

  if (Attrs.getKind() == ParsedAttr::AT_RISCVVLSCC) {
    // riscv_vls_cc only accepts 0 or 1 argument.
    if (!Attrs.checkAtLeastNumArgs(*this, 0) ||
        !Attrs.checkAtMostNumArgs(*this, 1)) {
      Attrs.setInvalid();
      return true;
    }
  } else {
    unsigned ReqArgs = Attrs.getKind() == ParsedAttr::AT_Pcs ? 1 : 0;
    if (!Attrs.checkExactlyNumArgs(*this, ReqArgs)) {
      Attrs.setInvalid();
      return true;
    }
  }

  bool IsTargetDefaultMSABI =
      Context.getTargetInfo().getTriple().isOSWindows() ||
      Context.getTargetInfo().getTriple().isUEFI();
  // TODO: diagnose uses of these conventions on the wrong target.
  switch (Attrs.getKind()) {
  case ParsedAttr::AT_CDecl:
    CC = CC_C;
    break;
  case ParsedAttr::AT_FastCall:
    CC = CC_X86FastCall;
    break;
  case ParsedAttr::AT_StdCall:
    CC = CC_X86StdCall;
    break;
  case ParsedAttr::AT_ThisCall:
    CC = CC_X86ThisCall;
    break;
  case ParsedAttr::AT_Pascal:
    CC = CC_X86Pascal;
    break;
  case ParsedAttr::AT_SwiftCall:
    CC = CC_Swift;
    break;
  case ParsedAttr::AT_SwiftAsyncCall:
    CC = CC_SwiftAsync;
    break;
  case ParsedAttr::AT_VectorCall:
    CC = CC_X86VectorCall;
    break;
  case ParsedAttr::AT_AArch64VectorPcs:
    CC = CC_AArch64VectorCall;
    break;
  case ParsedAttr::AT_AArch64SVEPcs:
    CC = CC_AArch64SVEPCS;
    break;
  case ParsedAttr::AT_RegCall:
    CC = CC_X86RegCall;
    break;
  case ParsedAttr::AT_MSABI:
    CC = IsTargetDefaultMSABI ? CC_C : CC_Win64;
    break;
  case ParsedAttr::AT_SysVABI:
    CC = IsTargetDefaultMSABI ? CC_X86_64SysV : CC_C;
    break;
  case ParsedAttr::AT_Pcs: {
    StringRef StrRef;
    if (!checkStringLiteralArgumentAttr(Attrs, 0, StrRef)) {
      Attrs.setInvalid();
      return true;
    }
    if (StrRef == "aapcs") {
      CC = CC_AAPCS;
      break;
    } else if (StrRef == "aapcs-vfp") {
      CC = CC_AAPCS_VFP;
      break;
    }

    Attrs.setInvalid();
    Diag(Attrs.getLoc(), diag::err_invalid_pcs);
    return true;
  }
  case ParsedAttr::AT_IntelOclBicc:
    CC = CC_IntelOclBicc;
    break;
  case ParsedAttr::AT_PreserveMost:
    CC = CC_PreserveMost;
    break;
  case ParsedAttr::AT_PreserveAll:
    CC = CC_PreserveAll;
    break;
  case ParsedAttr::AT_M68kRTD:
    CC = CC_M68kRTD;
    break;
  case ParsedAttr::AT_PreserveNone:
    CC = CC_PreserveNone;
    break;
  case ParsedAttr::AT_RISCVVectorCC:
    CC = CC_RISCVVectorCall;
    break;
  case ParsedAttr::AT_RISCVVLSCC: {
    // If the riscv_abi_vlen doesn't have any argument, we set set it to default
    // value 128.
    unsigned ABIVLen = 128;
    if (Attrs.getNumArgs() &&
        !checkUInt32Argument(Attrs, Attrs.getArgAsExpr(0), ABIVLen)) {
      Attrs.setInvalid();
      return true;
    }
    if (Attrs.getNumArgs() && (ABIVLen < 32 || ABIVLen > 65536)) {
      Attrs.setInvalid();
      Diag(Attrs.getLoc(), diag::err_argument_invalid_range)
          << ABIVLen << 32 << 65536;
      return true;
    }
    if (!llvm::isPowerOf2_64(ABIVLen)) {
      Attrs.setInvalid();
      Diag(Attrs.getLoc(), diag::err_argument_not_power_of_2);
      return true;
    }
    CC = static_cast<CallingConv>(CallingConv::CC_RISCVVLSCall_32 +
                                  llvm::Log2_64(ABIVLen) - 5);
    break;
  }
  case ParsedAttr::AT_DeviceKernel: {
    // Validation was handled in handleDeviceKernelAttr.
    CC = CC_DeviceKernel;
    break;
  }
  default: llvm_unreachable("unexpected attribute kind");
  }

  TargetInfo::CallingConvCheckResult A = TargetInfo::CCCR_OK;
  const TargetInfo &TI = Context.getTargetInfo();
  auto *Aux = Context.getAuxTargetInfo();
  // CUDA functions may have host and/or device attributes which indicate
  // their targeted execution environment, therefore the calling convention
  // of functions in CUDA should be checked against the target deduced based
  // on their host/device attributes.
  if (LangOpts.CUDA) {
    assert(FD || CFT != CUDAFunctionTarget::InvalidTarget);
    auto CudaTarget = FD ? CUDA().IdentifyTarget(FD) : CFT;
    bool CheckHost = false, CheckDevice = false;
    switch (CudaTarget) {
    case CUDAFunctionTarget::HostDevice:
      CheckHost = true;
      CheckDevice = true;
      break;
    case CUDAFunctionTarget::Host:
      CheckHost = true;
      break;
    case CUDAFunctionTarget::Device:
    case CUDAFunctionTarget::Global:
      CheckDevice = true;
      break;
    case CUDAFunctionTarget::InvalidTarget:
      llvm_unreachable("unexpected cuda target");
    }
    auto *HostTI = LangOpts.CUDAIsDevice ? Aux : &TI;
    auto *DeviceTI = LangOpts.CUDAIsDevice ? &TI : Aux;
    if (CheckHost && HostTI)
      A = HostTI->checkCallingConvention(CC);
    if (A == TargetInfo::CCCR_OK && CheckDevice && DeviceTI)
      A = DeviceTI->checkCallingConvention(CC);
  } else if (LangOpts.SYCLIsDevice && TI.getTriple().isAMDGPU() &&
             CC == CC_X86VectorCall) {
    // Assuming SYCL Device AMDGPU CC_X86VectorCall functions are always to be
    // emitted on the host. The MSVC STL has CC-based specializations so we
    // cannot change the CC to be the default as that will cause a clash with
    // another specialization.
    A = TI.checkCallingConvention(CC);
    if (Aux && A != TargetInfo::CCCR_OK)
      A = Aux->checkCallingConvention(CC);
  } else {
    A = TI.checkCallingConvention(CC);
  }

  switch (A) {
  case TargetInfo::CCCR_OK:
    break;

  case TargetInfo::CCCR_Ignore:
    // Treat an ignored convention as if it was an explicit C calling convention
    // attribute. For example, __stdcall on Win x64 functions as __cdecl, so
    // that command line flags that change the default convention to
    // __vectorcall don't affect declarations marked __stdcall.
    CC = CC_C;
    break;

  case TargetInfo::CCCR_Error:
    Diag(Attrs.getLoc(), diag::error_cconv_unsupported)
        << Attrs << (int)CallingConventionIgnoredReason::ForThisTarget;
    break;

  case TargetInfo::CCCR_Warning: {
    Diag(Attrs.getLoc(), diag::warn_cconv_unsupported)
        << Attrs << (int)CallingConventionIgnoredReason::ForThisTarget;

    // This convention is not valid for the target. Use the default function or
    // method calling convention.
    bool IsCXXMethod = false, IsVariadic = false;
    if (FD) {
      IsCXXMethod = FD->isCXXInstanceMember();
      IsVariadic = FD->isVariadic();
    }
    CC = Context.getDefaultCallingConvention(IsVariadic, IsCXXMethod);
    break;
  }
  }

  Attrs.setProcessingCache((unsigned) CC);
  return false;
}

bool Sema::CheckRegparmAttr(const ParsedAttr &AL, unsigned &numParams) {
  if (AL.isInvalid())
    return true;

  if (!AL.checkExactlyNumArgs(*this, 1)) {
    AL.setInvalid();
    return true;
  }

  uint32_t NP;
  Expr *NumParamsExpr = AL.getArgAsExpr(0);
  if (!checkUInt32Argument(AL, NumParamsExpr, NP)) {
    AL.setInvalid();
    return true;
  }

  if (Context.getTargetInfo().getRegParmMax() == 0) {
    Diag(AL.getLoc(), diag::err_attribute_regparm_wrong_platform)
      << NumParamsExpr->getSourceRange();
    AL.setInvalid();
    return true;
  }

  numParams = NP;
  if (numParams > Context.getTargetInfo().getRegParmMax()) {
    Diag(AL.getLoc(), diag::err_attribute_regparm_invalid_number)
      << Context.getTargetInfo().getRegParmMax() << NumParamsExpr->getSourceRange();
    AL.setInvalid();
    return true;
  }

  return false;
}

// Helper to get OffloadArch.
static OffloadArch getOffloadArch(const TargetInfo &TI) {
  if (!TI.getTriple().isNVPTX())
    llvm_unreachable("getOffloadArch is only valid for NVPTX triple");
  auto &TO = TI.getTargetOpts();
  return StringToOffloadArch(TO.CPU);
}

// Checks whether an argument of launch_bounds attribute is
// acceptable, performs implicit conversion to Rvalue, and returns
// non-nullptr Expr result on success. Otherwise, it returns nullptr
// and may output an error.
static Expr *makeLaunchBoundsArgExpr(Sema &S, Expr *E,
                                     const CUDALaunchBoundsAttr &AL,
                                     const unsigned Idx) {
  if (S.DiagnoseUnexpandedParameterPack(E))
    return nullptr;

  // Accept template arguments for now as they depend on something else.
  // We'll get to check them when they eventually get instantiated.
  if (E->isValueDependent())
    return E;

  std::optional<llvm::APSInt> I = llvm::APSInt(64);
  if (!(I = E->getIntegerConstantExpr(S.Context))) {
    S.Diag(E->getExprLoc(), diag::err_attribute_argument_n_type)
        << &AL << Idx << AANT_ArgumentIntegerConstant << E->getSourceRange();
    return nullptr;
  }
  // Make sure we can fit it in 32 bits.
  if (!I->isIntN(32)) {
    S.Diag(E->getExprLoc(), diag::err_ice_too_large)
        << toString(*I, 10, false) << 32 << /* Unsigned */ 1;
    return nullptr;
  }
  if (*I < 0)
    S.Diag(E->getExprLoc(), diag::warn_attribute_argument_n_negative)
        << &AL << Idx << E->getSourceRange();

  // We may need to perform implicit conversion of the argument.
  InitializedEntity Entity = InitializedEntity::InitializeParameter(
      S.Context, S.Context.getConstType(S.Context.IntTy), /*consume*/ false);
  ExprResult ValArg = S.PerformCopyInitialization(Entity, SourceLocation(), E);
  assert(!ValArg.isInvalid() &&
         "Unexpected PerformCopyInitialization() failure.");

  return ValArg.getAs<Expr>();
}

CUDALaunchBoundsAttr *
Sema::CreateLaunchBoundsAttr(const AttributeCommonInfo &CI, Expr *MaxThreads,
                             Expr *MinBlocks, Expr *MaxBlocks) {
  CUDALaunchBoundsAttr TmpAttr(Context, CI, MaxThreads, MinBlocks, MaxBlocks);
  MaxThreads = makeLaunchBoundsArgExpr(*this, MaxThreads, TmpAttr, 0);
  if (!MaxThreads)
    return nullptr;

  if (MinBlocks) {
    MinBlocks = makeLaunchBoundsArgExpr(*this, MinBlocks, TmpAttr, 1);
    if (!MinBlocks)
      return nullptr;
  }

  if (MaxBlocks) {
    // '.maxclusterrank' ptx directive requires .target sm_90 or higher.
    auto SM = getOffloadArch(Context.getTargetInfo());
    if (SM == OffloadArch::UNKNOWN || SM < OffloadArch::SM_90) {
      Diag(MaxBlocks->getBeginLoc(), diag::warn_cuda_maxclusterrank_sm_90)
          << OffloadArchToString(SM) << CI << MaxBlocks->getSourceRange();
      // Ignore it by setting MaxBlocks to null;
      MaxBlocks = nullptr;
    } else {
      MaxBlocks = makeLaunchBoundsArgExpr(*this, MaxBlocks, TmpAttr, 2);
      if (!MaxBlocks)
        return nullptr;
    }
  }

  return ::new (Context)
      CUDALaunchBoundsAttr(Context, CI, MaxThreads, MinBlocks, MaxBlocks);
}

void Sema::AddLaunchBoundsAttr(Decl *D, const AttributeCommonInfo &CI,
                               Expr *MaxThreads, Expr *MinBlocks,
                               Expr *MaxBlocks) {
  if (auto *Attr = CreateLaunchBoundsAttr(CI, MaxThreads, MinBlocks, MaxBlocks))
    D->addAttr(Attr);
}

static void handleLaunchBoundsAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!AL.checkAtLeastNumArgs(S, 1) || !AL.checkAtMostNumArgs(S, 3))
    return;

  S.AddLaunchBoundsAttr(D, AL, AL.getArgAsExpr(0),
                        AL.getNumArgs() > 1 ? AL.getArgAsExpr(1) : nullptr,
                        AL.getNumArgs() > 2 ? AL.getArgAsExpr(2) : nullptr);
}

static void handleArgumentWithTypeTagAttr(Sema &S, Decl *D,
                                          const ParsedAttr &AL) {
  if (!AL.isArgIdent(0)) {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_n_type)
        << AL << /* arg num = */ 1 << AANT_ArgumentIdentifier;
    return;
  }

  ParamIdx ArgumentIdx;
  if (!S.checkFunctionOrMethodParameterIndex(
          D, AL, 2, AL.getArgAsExpr(1), ArgumentIdx,
          /*CanIndexImplicitThis=*/false,
          /*CanIndexVariadicArguments=*/true))
    return;

  ParamIdx TypeTagIdx;
  if (!S.checkFunctionOrMethodParameterIndex(
          D, AL, 3, AL.getArgAsExpr(2), TypeTagIdx,
          /*CanIndexImplicitThis=*/false,
          /*CanIndexVariadicArguments=*/true))
    return;

  bool IsPointer = AL.getAttrName()->getName() == "pointer_with_type_tag";
  if (IsPointer) {
    // Ensure that buffer has a pointer type.
    unsigned ArgumentIdxAST = ArgumentIdx.getASTIndex();
    if (ArgumentIdxAST >= getFunctionOrMethodNumParams(D) ||
        !getFunctionOrMethodParamType(D, ArgumentIdxAST)->isPointerType())
      S.Diag(AL.getLoc(), diag::err_attribute_pointers_only) << AL << 0;
  }

  D->addAttr(::new (S.Context) ArgumentWithTypeTagAttr(
      S.Context, AL, AL.getArgAsIdent(0)->getIdentifierInfo(), ArgumentIdx,
      TypeTagIdx, IsPointer));
}

static void handleTypeTagForDatatypeAttr(Sema &S, Decl *D,
                                         const ParsedAttr &AL) {
  if (!AL.isArgIdent(0)) {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_n_type)
        << AL << 1 << AANT_ArgumentIdentifier;
    return;
  }

  if (!AL.checkExactlyNumArgs(S, 1))
    return;

  if (!isa<VarDecl>(D)) {
    S.Diag(AL.getLoc(), diag::err_attribute_wrong_decl_type)
        << AL << AL.isRegularKeywordAttribute() << ExpectedVariable;
    return;
  }

  IdentifierInfo *PointerKind = AL.getArgAsIdent(0)->getIdentifierInfo();
  TypeSourceInfo *MatchingCTypeLoc = nullptr;
  S.GetTypeFromParser(AL.getMatchingCType(), &MatchingCTypeLoc);
  assert(MatchingCTypeLoc && "no type source info for attribute argument");

  D->addAttr(::new (S.Context) TypeTagForDatatypeAttr(
      S.Context, AL, PointerKind, MatchingCTypeLoc, AL.getLayoutCompatible(),
      AL.getMustBeNull()));
}

static void handleXRayLogArgsAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  ParamIdx ArgCount;

  if (!S.checkFunctionOrMethodParameterIndex(D, AL, 1, AL.getArgAsExpr(0),
                                             ArgCount,
                                             true /* CanIndexImplicitThis */))
    return;

  // ArgCount isn't a parameter index [0;n), it's a count [1;n]
  D->addAttr(::new (S.Context)
                 XRayLogArgsAttr(S.Context, AL, ArgCount.getSourceIndex()));
}

static void handlePatchableFunctionEntryAttr(Sema &S, Decl *D,
                                             const ParsedAttr &AL) {
  if (S.Context.getTargetInfo().getTriple().isOSAIX()) {
    S.Diag(AL.getLoc(), diag::err_aix_attr_unsupported) << AL;
    return;
  }
  uint32_t Count = 0, Offset = 0;
  StringRef Section;
  if (!S.checkUInt32Argument(AL, AL.getArgAsExpr(0), Count, 0, true))
    return;
  if (AL.getNumArgs() >= 2) {
    Expr *Arg = AL.getArgAsExpr(1);
    if (!S.checkUInt32Argument(AL, Arg, Offset, 1, true))
      return;
    if (Count < Offset) {
      S.Diag(S.getAttrLoc(AL), diag::err_attribute_argument_out_of_range)
          << &AL << 0 << Count << Arg->getBeginLoc();
      return;
    }
  }
  if (AL.getNumArgs() == 3) {
    SourceLocation LiteralLoc;
    if (!S.checkStringLiteralArgumentAttr(AL, 2, Section, &LiteralLoc))
      return;
    if (llvm::Error E = S.isValidSectionSpecifier(Section)) {
      S.Diag(LiteralLoc,
             diag::err_attribute_patchable_function_entry_invalid_section)
          << toString(std::move(E));
      return;
    }
    if (Section.empty()) {
      S.Diag(LiteralLoc,
             diag::err_attribute_patchable_function_entry_invalid_section)
          << "section must not be empty";
      return;
    }
  }
  D->addAttr(::new (S.Context) PatchableFunctionEntryAttr(S.Context, AL, Count,
                                                          Offset, Section));
}

static void handleBuiltinAliasAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!AL.isArgIdent(0)) {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_n_type)
        << AL << 1 << AANT_ArgumentIdentifier;
    return;
  }

  IdentifierInfo *Ident = AL.getArgAsIdent(0)->getIdentifierInfo();
  unsigned BuiltinID = Ident->getBuiltinID();
  StringRef AliasName = cast<FunctionDecl>(D)->getIdentifier()->getName();

  bool IsAArch64 = S.Context.getTargetInfo().getTriple().isAArch64();
  bool IsARM = S.Context.getTargetInfo().getTriple().isARM();
  bool IsRISCV = S.Context.getTargetInfo().getTriple().isRISCV();
  bool IsSPIRV = S.Context.getTargetInfo().getTriple().isSPIRV();
  bool IsHLSL = S.Context.getLangOpts().HLSL;
  if ((IsAArch64 && !S.ARM().SveAliasValid(BuiltinID, AliasName)) ||
      (IsARM && !S.ARM().MveAliasValid(BuiltinID, AliasName) &&
       !S.ARM().CdeAliasValid(BuiltinID, AliasName)) ||
      (IsRISCV && !S.RISCV().isAliasValid(BuiltinID, AliasName)) ||
      (!IsAArch64 && !IsARM && !IsRISCV && !IsHLSL && !IsSPIRV)) {
    S.Diag(AL.getLoc(), diag::err_attribute_builtin_alias) << AL;
    return;
  }

  D->addAttr(::new (S.Context) BuiltinAliasAttr(S.Context, AL, Ident));
}

static void handleNullableTypeAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (AL.isUsedAsTypeAttr())
    return;

  if (auto *CRD = dyn_cast<CXXRecordDecl>(D);
      !CRD || !(CRD->isClass() || CRD->isStruct())) {
    S.Diag(AL.getRange().getBegin(), diag::err_attribute_wrong_decl_type)
        << AL << AL.isRegularKeywordAttribute() << ExpectedClass;
    return;
  }

  handleSimpleAttribute<TypeNullableAttr>(S, D, AL);
}

static void handlePreferredTypeAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!AL.hasParsedType()) {
    S.Diag(AL.getLoc(), diag::err_attribute_wrong_number_arguments) << AL << 1;
    return;
  }

  TypeSourceInfo *ParmTSI = nullptr;
  QualType QT = S.GetTypeFromParser(AL.getTypeArg(), &ParmTSI);
  assert(ParmTSI && "no type source info for attribute argument");
  S.RequireCompleteType(ParmTSI->getTypeLoc().getBeginLoc(), QT,
                        diag::err_incomplete_type);

  D->addAttr(::new (S.Context) PreferredTypeAttr(S.Context, AL, ParmTSI));
}

//===----------------------------------------------------------------------===//
// Microsoft specific attribute handlers.
//===----------------------------------------------------------------------===//

UuidAttr *Sema::mergeUuidAttr(Decl *D, const AttributeCommonInfo &CI,
                              StringRef UuidAsWritten, MSGuidDecl *GuidDecl) {
  if (const auto *UA = D->getAttr<UuidAttr>()) {
    if (declaresSameEntity(UA->getGuidDecl(), GuidDecl))
      return nullptr;
    if (!UA->getGuid().empty()) {
      Diag(UA->getLocation(), diag::err_mismatched_uuid);
      Diag(CI.getLoc(), diag::note_previous_uuid);
      D->dropAttr<UuidAttr>();
    }
  }

  return ::new (Context) UuidAttr(Context, CI, UuidAsWritten, GuidDecl);
}

static void handleUuidAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!S.LangOpts.CPlusPlus) {
    S.Diag(AL.getLoc(), diag::err_attribute_not_supported_in_lang)
        << AL << AttributeLangSupport::C;
    return;
  }

  StringRef OrigStrRef;
  SourceLocation LiteralLoc;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, OrigStrRef, &LiteralLoc))
    return;

  // GUID format is "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" or
  // "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}", normalize to the former.
  StringRef StrRef = OrigStrRef;
  if (StrRef.size() == 38 && StrRef.front() == '{' && StrRef.back() == '}')
    StrRef = StrRef.drop_front().drop_back();

  // Validate GUID length.
  if (StrRef.size() != 36) {
    S.Diag(LiteralLoc, diag::err_attribute_uuid_malformed_guid);
    return;
  }

  for (unsigned i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (StrRef[i] != '-') {
        S.Diag(LiteralLoc, diag::err_attribute_uuid_malformed_guid);
        return;
      }
    } else if (!isHexDigit(StrRef[i])) {
      S.Diag(LiteralLoc, diag::err_attribute_uuid_malformed_guid);
      return;
    }
  }

  // Convert to our parsed format and canonicalize.
  MSGuidDecl::Parts Parsed;
  StrRef.substr(0, 8).getAsInteger(16, Parsed.Part1);
  StrRef.substr(9, 4).getAsInteger(16, Parsed.Part2);
  StrRef.substr(14, 4).getAsInteger(16, Parsed.Part3);
  for (unsigned i = 0; i != 8; ++i)
    StrRef.substr(19 + 2 * i + (i >= 2 ? 1 : 0), 2)
        .getAsInteger(16, Parsed.Part4And5[i]);
  MSGuidDecl *Guid = S.Context.getMSGuidDecl(Parsed);

  // FIXME: It'd be nice to also emit a fixit removing uuid(...) (and, if it's
  // the only thing in the [] list, the [] too), and add an insertion of
  // __declspec(uuid(...)).  But sadly, neither the SourceLocs of the commas
  // separating attributes nor of the [ and the ] are in the AST.
  // Cf "SourceLocations of attribute list delimiters - [[ ... , ... ]] etc"
  // on cfe-dev.
  if (AL.isMicrosoftAttribute()) // Check for [uuid(...)] spelling.
    S.Diag(AL.getLoc(), diag::warn_atl_uuid_deprecated);

  UuidAttr *UA = S.mergeUuidAttr(D, AL, OrigStrRef, Guid);
  if (UA)
    D->addAttr(UA);
}

static void handleMSInheritanceAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!S.LangOpts.CPlusPlus) {
    S.Diag(AL.getLoc(), diag::err_attribute_not_supported_in_lang)
        << AL << AttributeLangSupport::C;
    return;
  }
  MSInheritanceAttr *IA = S.mergeMSInheritanceAttr(
      D, AL, /*BestCase=*/true, (MSInheritanceModel)AL.getSemanticSpelling());
  if (IA) {
    D->addAttr(IA);
    S.Consumer.AssignInheritanceModel(cast<CXXRecordDecl>(D));
  }
}

static void handleDeclspecThreadAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  const auto *VD = cast<VarDecl>(D);
  if (!S.Context.getTargetInfo().isTLSSupported()) {
    S.Diag(AL.getLoc(), diag::err_thread_unsupported);
    return;
  }
  if (VD->getTSCSpec() != TSCS_unspecified) {
    S.Diag(AL.getLoc(), diag::err_declspec_thread_on_thread_variable);
    return;
  }
  if (VD->hasLocalStorage()) {
    S.Diag(AL.getLoc(), diag::err_thread_non_global) << "__declspec(thread)";
    return;
  }
  D->addAttr(::new (S.Context) ThreadAttr(S.Context, AL));
}

static void handleMSConstexprAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!S.getLangOpts().isCompatibleWithMSVC(LangOptions::MSVC2022_3)) {
    S.Diag(AL.getLoc(), diag::warn_unknown_attribute_ignored)
        << AL << AL.getRange();
    return;
  }
  auto *FD = cast<FunctionDecl>(D);
  if (FD->isConstexprSpecified() || FD->isConsteval()) {
    S.Diag(AL.getLoc(), diag::err_ms_constexpr_cannot_be_applied)
        << FD->isConsteval() << FD;
    return;
  }
  if (auto *MD = dyn_cast<CXXMethodDecl>(FD)) {
    if (!S.getLangOpts().CPlusPlus20 && MD->isVirtual()) {
      S.Diag(AL.getLoc(), diag::err_ms_constexpr_cannot_be_applied)
          << /*virtual*/ 2 << MD;
      return;
    }
  }
  D->addAttr(::new (S.Context) MSConstexprAttr(S.Context, AL));
}

static void handleAbiTagAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  SmallVector<StringRef, 4> Tags;
  for (unsigned I = 0, E = AL.getNumArgs(); I != E; ++I) {
    StringRef Tag;
    if (!S.checkStringLiteralArgumentAttr(AL, I, Tag))
      return;
    Tags.push_back(Tag);
  }

  if (const auto *NS = dyn_cast<NamespaceDecl>(D)) {
    if (!NS->isInline()) {
      S.Diag(AL.getLoc(), diag::warn_attr_abi_tag_namespace) << 0;
      return;
    }
    if (NS->isAnonymousNamespace()) {
      S.Diag(AL.getLoc(), diag::warn_attr_abi_tag_namespace) << 1;
      return;
    }
    if (AL.getNumArgs() == 0)
      Tags.push_back(NS->getName());
  } else if (!AL.checkAtLeastNumArgs(S, 1))
    return;

  // Store tags sorted and without duplicates.
  llvm::sort(Tags);
  Tags.erase(llvm::unique(Tags), Tags.end());

  D->addAttr(::new (S.Context)
                 AbiTagAttr(S.Context, AL, Tags.data(), Tags.size()));
}

static bool hasBTFDeclTagAttr(Decl *D, StringRef Tag) {
  for (const auto *I : D->specific_attrs<BTFDeclTagAttr>()) {
    if (I->getBTFDeclTag() == Tag)
      return true;
  }
  return false;
}

static void handleBTFDeclTagAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  StringRef Str;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, Str))
    return;
  if (hasBTFDeclTagAttr(D, Str))
    return;

  D->addAttr(::new (S.Context) BTFDeclTagAttr(S.Context, AL, Str));
}

BTFDeclTagAttr *Sema::mergeBTFDeclTagAttr(Decl *D, const BTFDeclTagAttr &AL) {
  if (hasBTFDeclTagAttr(D, AL.getBTFDeclTag()))
    return nullptr;
  return ::new (Context) BTFDeclTagAttr(Context, AL, AL.getBTFDeclTag());
}

static void handleInterruptAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // Dispatch the interrupt attribute based on the current target.
  switch (S.Context.getTargetInfo().getTriple().getArch()) {
  case llvm::Triple::msp430:
    S.MSP430().handleInterruptAttr(D, AL);
    break;
  case llvm::Triple::mipsel:
  case llvm::Triple::mips:
    S.MIPS().handleInterruptAttr(D, AL);
    break;
  case llvm::Triple::m68k:
    S.M68k().handleInterruptAttr(D, AL);
    break;
  case llvm::Triple::x86:
  case llvm::Triple::x86_64:
    S.X86().handleAnyInterruptAttr(D, AL);
    break;
  case llvm::Triple::avr:
    S.AVR().handleInterruptAttr(D, AL);
    break;
  case llvm::Triple::riscv32:
  case llvm::Triple::riscv64:
    S.RISCV().handleInterruptAttr(D, AL);
    break;
  default:
    S.ARM().handleInterruptAttr(D, AL);
    break;
  }
}

static void handleLayoutVersion(Sema &S, Decl *D, const ParsedAttr &AL) {
  uint32_t Version;
  Expr *VersionExpr = static_cast<Expr *>(AL.getArgAsExpr(0));
  if (!S.checkUInt32Argument(AL, AL.getArgAsExpr(0), Version))
    return;

  // TODO: Investigate what happens with the next major version of MSVC.
  if (Version != LangOptions::MSVC2015 / 100) {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_out_of_bounds)
        << AL << Version << VersionExpr->getSourceRange();
    return;
  }

  // The attribute expects a "major" version number like 19, but new versions of
  // MSVC have moved to updating the "minor", or less significant numbers, so we
  // have to multiply by 100 now.
  Version *= 100;

  D->addAttr(::new (S.Context) LayoutVersionAttr(S.Context, AL, Version));
}

DLLImportAttr *Sema::mergeDLLImportAttr(Decl *D,
                                        const AttributeCommonInfo &CI) {
  if (D->hasAttr<DLLExportAttr>()) {
    Diag(CI.getLoc(), diag::warn_attribute_ignored) << "'dllimport'";
    return nullptr;
  }

  if (D->hasAttr<DLLImportAttr>())
    return nullptr;

  return ::new (Context) DLLImportAttr(Context, CI);
}

DLLExportAttr *Sema::mergeDLLExportAttr(Decl *D,
                                        const AttributeCommonInfo &CI) {
  if (DLLImportAttr *Import = D->getAttr<DLLImportAttr>()) {
    Diag(Import->getLocation(), diag::warn_attribute_ignored) << Import;
    D->dropAttr<DLLImportAttr>();
  }

  if (D->hasAttr<DLLExportAttr>())
    return nullptr;

  return ::new (Context) DLLExportAttr(Context, CI);
}

static void handleDLLAttr(Sema &S, Decl *D, const ParsedAttr &A) {
  if (isa<ClassTemplatePartialSpecializationDecl>(D) &&
      (S.Context.getTargetInfo().shouldDLLImportComdatSymbols())) {
    S.Diag(A.getRange().getBegin(), diag::warn_attribute_ignored) << A;
    return;
  }

  if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
    if (FD->isInlined() && A.getKind() == ParsedAttr::AT_DLLImport &&
        !(S.Context.getTargetInfo().shouldDLLImportComdatSymbols())) {
      // MinGW doesn't allow dllimport on inline functions.
      S.Diag(A.getRange().getBegin(), diag::warn_attribute_ignored_on_inline)
          << A;
      return;
    }
  }

  if (const auto *MD = dyn_cast<CXXMethodDecl>(D)) {
    if ((S.Context.getTargetInfo().shouldDLLImportComdatSymbols()) &&
        MD->getParent()->isLambda()) {
      S.Diag(A.getRange().getBegin(), diag::err_attribute_dll_lambda) << A;
      return;
    }
  }

  Attr *NewAttr = A.getKind() == ParsedAttr::AT_DLLExport
                      ? (Attr *)S.mergeDLLExportAttr(D, A)
                      : (Attr *)S.mergeDLLImportAttr(D, A);
  if (NewAttr)
    D->addAttr(NewAttr);
}

MSInheritanceAttr *
Sema::mergeMSInheritanceAttr(Decl *D, const AttributeCommonInfo &CI,
                             bool BestCase,
                             MSInheritanceModel Model) {
  if (MSInheritanceAttr *IA = D->getAttr<MSInheritanceAttr>()) {
    if (IA->getInheritanceModel() == Model)
      return nullptr;
    Diag(IA->getLocation(), diag::err_mismatched_ms_inheritance)
        << 1 /*previous declaration*/;
    Diag(CI.getLoc(), diag::note_previous_ms_inheritance);
    D->dropAttr<MSInheritanceAttr>();
  }

  auto *RD = cast<CXXRecordDecl>(D);
  if (RD->hasDefinition()) {
    if (checkMSInheritanceAttrOnDefinition(RD, CI.getRange(), BestCase,
                                           Model)) {
      return nullptr;
    }
  } else {
    if (isa<ClassTemplatePartialSpecializationDecl>(RD)) {
      Diag(CI.getLoc(), diag::warn_ignored_ms_inheritance)
          << 1 /*partial specialization*/;
      return nullptr;
    }
    if (RD->getDescribedClassTemplate()) {
      Diag(CI.getLoc(), diag::warn_ignored_ms_inheritance)
          << 0 /*primary template*/;
      return nullptr;
    }
  }

  return ::new (Context) MSInheritanceAttr(Context, CI, BestCase);
}

static void handleCapabilityAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // The capability attributes take a single string parameter for the name of
  // the capability they represent. The lockable attribute does not take any
  // parameters. However, semantically, both attributes represent the same
  // concept, and so they use the same semantic attribute. Eventually, the
  // lockable attribute will be removed.
  //
  // For backward compatibility, any capability which has no specified string
  // literal will be considered a "mutex."
  StringRef N("mutex");
  SourceLocation LiteralLoc;
  if (AL.getKind() == ParsedAttr::AT_Capability &&
      !S.checkStringLiteralArgumentAttr(AL, 0, N, &LiteralLoc))
    return;

  D->addAttr(::new (S.Context) CapabilityAttr(S.Context, AL, N));
}

static void handleReentrantCapabilityAttr(Sema &S, Decl *D,
                                          const ParsedAttr &AL) {
  // Do not permit 'reentrant_capability' without 'capability(..)'. Note that
  // the check here requires 'capability' to be before 'reentrant_capability'.
  // This helps enforce a canonical style. Also avoids placing an additional
  // branch into ProcessDeclAttributeList().
  if (!D->hasAttr<CapabilityAttr>()) {
    S.Diag(AL.getLoc(), diag::warn_thread_attribute_requires_preceded)
        << AL << cast<NamedDecl>(D) << "'capability'";
    return;
  }

  D->addAttr(::new (S.Context) ReentrantCapabilityAttr(S.Context, AL));
}

static void handleAssertCapabilityAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  SmallVector<Expr*, 1> Args;
  if (!checkLockFunAttrCommon(S, D, AL, Args))
    return;

  D->addAttr(::new (S.Context)
                 AssertCapabilityAttr(S.Context, AL, Args.data(), Args.size()));
}

static void handleAcquireCapabilityAttr(Sema &S, Decl *D,
                                        const ParsedAttr &AL) {
  if (const auto *ParmDecl = dyn_cast<ParmVarDecl>(D);
      ParmDecl && !checkFunParamsAreScopedLockable(S, ParmDecl, AL))
    return;

  SmallVector<Expr*, 1> Args;
  if (!checkLockFunAttrCommon(S, D, AL, Args))
    return;

  D->addAttr(::new (S.Context) AcquireCapabilityAttr(S.Context, AL, Args.data(),
                                                     Args.size()));
}

static void handleTryAcquireCapabilityAttr(Sema &S, Decl *D,
                                           const ParsedAttr &AL) {
  SmallVector<Expr*, 2> Args;
  if (!checkTryLockFunAttrCommon(S, D, AL, Args))
    return;

  D->addAttr(::new (S.Context) TryAcquireCapabilityAttr(
      S.Context, AL, AL.getArgAsExpr(0), Args.data(), Args.size()));
}

static void handleReleaseCapabilityAttr(Sema &S, Decl *D,
                                        const ParsedAttr &AL) {
  if (const auto *ParmDecl = dyn_cast<ParmVarDecl>(D);
      ParmDecl && !checkFunParamsAreScopedLockable(S, ParmDecl, AL))
    return;
  // Check that all arguments are lockable objects.
  SmallVector<Expr *, 1> Args;
  checkAttrArgsAreCapabilityObjs(S, D, AL, Args, 0, true);

  D->addAttr(::new (S.Context) ReleaseCapabilityAttr(S.Context, AL, Args.data(),
                                                     Args.size()));
}

static void handleRequiresCapabilityAttr(Sema &S, Decl *D,
                                         const ParsedAttr &AL) {
  if (const auto *ParmDecl = dyn_cast<ParmVarDecl>(D);
      ParmDecl && !checkFunParamsAreScopedLockable(S, ParmDecl, AL))
    return;

  if (!AL.checkAtLeastNumArgs(S, 1))
    return;

  // check that all arguments are lockable objects
  SmallVector<Expr*, 1> Args;
  checkAttrArgsAreCapabilityObjs(S, D, AL, Args);
  if (Args.empty())
    return;

  RequiresCapabilityAttr *RCA = ::new (S.Context)
      RequiresCapabilityAttr(S.Context, AL, Args.data(), Args.size());

  D->addAttr(RCA);
}

static void handleDeprecatedAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (const auto *NSD = dyn_cast<NamespaceDecl>(D)) {
    if (NSD->isAnonymousNamespace()) {
      S.Diag(AL.getLoc(), diag::warn_deprecated_anonymous_namespace);
      // Do not want to attach the attribute to the namespace because that will
      // cause confusing diagnostic reports for uses of declarations within the
      // namespace.
      return;
    }
  } else if (isa<UsingDecl, UnresolvedUsingTypenameDecl,
                 UnresolvedUsingValueDecl>(D)) {
    S.Diag(AL.getRange().getBegin(), diag::warn_deprecated_ignored_on_using)
        << AL;
    return;
  }

  // Handle the cases where the attribute has a text message.
  StringRef Str, Replacement;
  if (AL.isArgExpr(0) && AL.getArgAsExpr(0) &&
      !S.checkStringLiteralArgumentAttr(AL, 0, Str))
    return;

  // Support a single optional message only for Declspec and [[]] spellings.
  if (AL.isDeclspecAttribute() || AL.isStandardAttributeSyntax())
    AL.checkAtMostNumArgs(S, 1);
  else if (AL.isArgExpr(1) && AL.getArgAsExpr(1) &&
           !S.checkStringLiteralArgumentAttr(AL, 1, Replacement))
    return;

  if (!S.getLangOpts().CPlusPlus14 && AL.isCXX11Attribute() && !AL.isGNUScope())
    S.Diag(AL.getLoc(), diag::ext_cxx14_attr) << AL;

  D->addAttr(::new (S.Context) DeprecatedAttr(S.Context, AL, Str, Replacement));
}

static bool isGlobalVar(const Decl *D) {
  if (const auto *S = dyn_cast<VarDecl>(D))
    return S->hasGlobalStorage();
  return false;
}

static bool isSanitizerAttributeAllowedOnGlobals(StringRef Sanitizer) {
  return Sanitizer == "address" || Sanitizer == "hwaddress" ||
         Sanitizer == "memtag";
}

static void handleNoSanitizeAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (!AL.checkAtLeastNumArgs(S, 1))
    return;

  std::vector<StringRef> Sanitizers;

  for (unsigned I = 0, E = AL.getNumArgs(); I != E; ++I) {
    StringRef SanitizerName;
    SourceLocation LiteralLoc;

    if (!S.checkStringLiteralArgumentAttr(AL, I, SanitizerName, &LiteralLoc))
      return;

    if (parseSanitizerValue(SanitizerName, /*AllowGroups=*/true) ==
            SanitizerMask() &&
        SanitizerName != "coverage")
      S.Diag(LiteralLoc, diag::warn_unknown_sanitizer_ignored) << SanitizerName;
    else if (isGlobalVar(D) && !isSanitizerAttributeAllowedOnGlobals(SanitizerName))
      S.Diag(D->getLocation(), diag::warn_attribute_type_not_supported_global)
          << AL << SanitizerName;
    Sanitizers.push_back(SanitizerName);
  }

  D->addAttr(::new (S.Context) NoSanitizeAttr(S.Context, AL, Sanitizers.data(),
                                              Sanitizers.size()));
}

static void handleNoSanitizeSpecificAttr(Sema &S, Decl *D,
                                         const ParsedAttr &AL) {
  StringRef AttrName = AL.getAttrName()->getName();
  normalizeName(AttrName);
  StringRef SanitizerName = llvm::StringSwitch<StringRef>(AttrName)
                                .Case("no_address_safety_analysis", "address")
                                .Case("no_sanitize_address", "address")
                                .Case("no_sanitize_thread", "thread")
                                .Case("no_sanitize_memory", "memory");
  if (isGlobalVar(D) && SanitizerName != "address")
    S.Diag(D->getLocation(), diag::err_attribute_wrong_decl_type)
        << AL << AL.isRegularKeywordAttribute() << ExpectedFunction;

  // FIXME: Rather than create a NoSanitizeSpecificAttr, this creates a
  // NoSanitizeAttr object; but we need to calculate the correct spelling list
  // index rather than incorrectly assume the index for NoSanitizeSpecificAttr
  // has the same spellings as the index for NoSanitizeAttr. We don't have a
  // general way to "translate" between the two, so this hack attempts to work
  // around the issue with hard-coded indices. This is critical for calling
  // getSpelling() or prettyPrint() on the resulting semantic attribute object
  // without failing assertions.
  unsigned TranslatedSpellingIndex = 0;
  if (AL.isStandardAttributeSyntax())
    TranslatedSpellingIndex = 1;

  AttributeCommonInfo Info = AL;
  Info.setAttributeSpellingListIndex(TranslatedSpellingIndex);
  D->addAttr(::new (S.Context)
                 NoSanitizeAttr(S.Context, Info, &SanitizerName, 1));
}

static void handleInternalLinkageAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (InternalLinkageAttr *Internal = S.mergeInternalLinkageAttr(D, AL))
    D->addAttr(Internal);
}

static void handleZeroCallUsedRegsAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // Check that the argument is a string literal.
  StringRef KindStr;
  SourceLocation LiteralLoc;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, KindStr, &LiteralLoc))
    return;

  ZeroCallUsedRegsAttr::ZeroCallUsedRegsKind Kind;
  if (!ZeroCallUsedRegsAttr::ConvertStrToZeroCallUsedRegsKind(KindStr, Kind)) {
    S.Diag(LiteralLoc, diag::warn_attribute_type_not_supported)
        << AL << KindStr;
    return;
  }

  D->dropAttr<ZeroCallUsedRegsAttr>();
  D->addAttr(ZeroCallUsedRegsAttr::Create(S.Context, Kind, AL));
}

static void handleCountedByAttrField(Sema &S, Decl *D, const ParsedAttr &AL) {
  auto *FD = dyn_cast<FieldDecl>(D);
  assert(FD);

  auto *CountExpr = AL.getArgAsExpr(0);
  if (!CountExpr)
    return;

  bool CountInBytes;
  bool OrNull;
  switch (AL.getKind()) {
  case ParsedAttr::AT_CountedBy:
    CountInBytes = false;
    OrNull = false;
    break;
  case ParsedAttr::AT_CountedByOrNull:
    CountInBytes = false;
    OrNull = true;
    break;
  case ParsedAttr::AT_SizedBy:
    CountInBytes = true;
    OrNull = false;
    break;
  case ParsedAttr::AT_SizedByOrNull:
    CountInBytes = true;
    OrNull = true;
    break;
  default:
    llvm_unreachable("unexpected counted_by family attribute");
  }

  if (S.CheckCountedByAttrOnField(FD, CountExpr, CountInBytes, OrNull))
    return;

  QualType CAT = S.BuildCountAttributedArrayOrPointerType(
      FD->getType(), CountExpr, CountInBytes, OrNull);
  FD->setType(CAT);
}

static void handleFunctionReturnThunksAttr(Sema &S, Decl *D,
                                           const ParsedAttr &AL) {
  StringRef KindStr;
  SourceLocation LiteralLoc;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, KindStr, &LiteralLoc))
    return;

  FunctionReturnThunksAttr::Kind Kind;
  if (!FunctionReturnThunksAttr::ConvertStrToKind(KindStr, Kind)) {
    S.Diag(LiteralLoc, diag::warn_attribute_type_not_supported)
        << AL << KindStr;
    return;
  }
  // FIXME: it would be good to better handle attribute merging rather than
  // silently replacing the existing attribute, so long as it does not break
  // the expected codegen tests.
  D->dropAttr<FunctionReturnThunksAttr>();
  D->addAttr(FunctionReturnThunksAttr::Create(S.Context, Kind, AL));
}

static void handleAvailableOnlyInDefaultEvalMethod(Sema &S, Decl *D,
                                                   const ParsedAttr &AL) {
  assert(isa<TypedefNameDecl>(D) && "This attribute only applies to a typedef");
  handleSimpleAttribute<AvailableOnlyInDefaultEvalMethodAttr>(S, D, AL);
}

static void handleNoMergeAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  auto *VDecl = dyn_cast<VarDecl>(D);
  if (VDecl && !VDecl->isFunctionPointerType()) {
    S.Diag(AL.getLoc(), diag::warn_attribute_ignored_non_function_pointer)
        << AL << VDecl;
    return;
  }
  D->addAttr(NoMergeAttr::Create(S.Context, AL));
}

static void handleNoUniqueAddressAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  D->addAttr(NoUniqueAddressAttr::Create(S.Context, AL));
}

static void handleDestroyAttr(Sema &S, Decl *D, const ParsedAttr &A) {
  if (!cast<VarDecl>(D)->hasGlobalStorage()) {
    S.Diag(D->getLocation(), diag::err_destroy_attr_on_non_static_var)
        << (A.getKind() == ParsedAttr::AT_AlwaysDestroy);
    return;
  }

  if (A.getKind() == ParsedAttr::AT_AlwaysDestroy)
    handleSimpleAttribute<AlwaysDestroyAttr>(S, D, A);
  else
    handleSimpleAttribute<NoDestroyAttr>(S, D, A);
}

static void handleUninitializedAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  assert(cast<VarDecl>(D)->getStorageDuration() == SD_Automatic &&
         "uninitialized is only valid on automatic duration variables");
  D->addAttr(::new (S.Context) UninitializedAttr(S.Context, AL));
}

static void handleMIGServerRoutineAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // Check that the return type is a `typedef int kern_return_t` or a typedef
  // around it, because otherwise MIG convention checks make no sense.
  // BlockDecl doesn't store a return type, so it's annoying to check,
  // so let's skip it for now.
  if (!isa<BlockDecl>(D)) {
    QualType T = getFunctionOrMethodResultType(D);
    bool IsKernReturnT = false;
    while (const auto *TT = T->getAs<TypedefType>()) {
      IsKernReturnT = (TT->getDecl()->getName() == "kern_return_t");
      T = TT->desugar();
    }
    if (!IsKernReturnT || T.getCanonicalType() != S.getASTContext().IntTy) {
      S.Diag(D->getBeginLoc(),
             diag::warn_mig_server_routine_does_not_return_kern_return_t);
      return;
    }
  }

  handleSimpleAttribute<MIGServerRoutineAttr>(S, D, AL);
}

static void handleMSAllocatorAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // Warn if the return type is not a pointer or reference type.
  if (auto *FD = dyn_cast<FunctionDecl>(D)) {
    QualType RetTy = FD->getReturnType();
    if (!RetTy->isPointerOrReferenceType()) {
      S.Diag(AL.getLoc(), diag::warn_declspec_allocator_nonpointer)
          << AL.getRange() << RetTy;
      return;
    }
  }

  handleSimpleAttribute<MSAllocatorAttr>(S, D, AL);
}

static void handleAcquireHandleAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  if (AL.isUsedAsTypeAttr())
    return;
  // Warn if the parameter is definitely not an output parameter.
  if (const auto *PVD = dyn_cast<ParmVarDecl>(D)) {
    if (PVD->getType()->isIntegerType()) {
      S.Diag(AL.getLoc(), diag::err_attribute_output_parameter)
          << AL.getRange();
      return;
    }
  }
  StringRef Argument;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, Argument))
    return;
  D->addAttr(AcquireHandleAttr::Create(S.Context, Argument, AL));
}

template<typename Attr>
static void handleHandleAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  StringRef Argument;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, Argument))
    return;
  D->addAttr(Attr::Create(S.Context, Argument, AL));
}

template<typename Attr>
static void handleUnsafeBufferUsage(Sema &S, Decl *D, const ParsedAttr &AL) {
  D->addAttr(Attr::Create(S.Context, AL));
}

static void handleCFGuardAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  // The guard attribute takes a single identifier argument.

  if (!AL.isArgIdent(0)) {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_type)
        << AL << AANT_ArgumentIdentifier;
    return;
  }

  CFGuardAttr::GuardArg Arg;
  IdentifierInfo *II = AL.getArgAsIdent(0)->getIdentifierInfo();
  if (!CFGuardAttr::ConvertStrToGuardArg(II->getName(), Arg)) {
    S.Diag(AL.getLoc(), diag::warn_attribute_type_not_supported) << AL << II;
    return;
  }

  D->addAttr(::new (S.Context) CFGuardAttr(S.Context, AL, Arg));
}


template <typename AttrTy>
static const AttrTy *findEnforceTCBAttrByName(Decl *D, StringRef Name) {
  auto Attrs = D->specific_attrs<AttrTy>();
  auto I = llvm::find_if(Attrs,
                         [Name](const AttrTy *A) {
                           return A->getTCBName() == Name;
                         });
  return I == Attrs.end() ? nullptr : *I;
}

template <typename AttrTy, typename ConflictingAttrTy>
static void handleEnforceTCBAttr(Sema &S, Decl *D, const ParsedAttr &AL) {
  StringRef Argument;
  if (!S.checkStringLiteralArgumentAttr(AL, 0, Argument))
    return;

  // A function cannot be have both regular and leaf membership in the same TCB.
  if (const ConflictingAttrTy *ConflictingAttr =
      findEnforceTCBAttrByName<ConflictingAttrTy>(D, Argument)) {
    // We could attach a note to the other attribute but in this case
    // there's no need given how the two are very close to each other.
    S.Diag(AL.getLoc(), diag::err_tcb_conflicting_attributes)
      << AL.getAttrName()->getName() << ConflictingAttr->getAttrName()->getName()
      << Argument;

    // Error recovery: drop the non-leaf attribute so that to suppress
    // all future warnings caused by erroneous attributes. The leaf attribute
    // needs to be kept because it can only suppresses warnings, not cause them.
    D->dropAttr<EnforceTCBAttr>();
    return;
  }

  D->addAttr(AttrTy::Create(S.Context, Argument, AL));
}

template <typename AttrTy, typename ConflictingAttrTy>
static AttrTy *mergeEnforceTCBAttrImpl(Sema &S, Decl *D, const AttrTy &AL) {
  // Check if the new redeclaration has different leaf-ness in the same TCB.
  StringRef TCBName = AL.getTCBName();
  if (const ConflictingAttrTy *ConflictingAttr =
      findEnforceTCBAttrByName<ConflictingAttrTy>(D, TCBName)) {
    S.Diag(ConflictingAttr->getLoc(), diag::err_tcb_conflicting_attributes)
      << ConflictingAttr->getAttrName()->getName()
      << AL.getAttrName()->getName() << TCBName;

    // Add a note so that the user could easily find the conflicting attribute.
    S.Diag(AL.getLoc(), diag::note_conflicting_attribute);

    // More error recovery.
    D->dropAttr<EnforceTCBAttr>();
    return nullptr;
  }

  ASTContext &Context = S.getASTContext();
  return ::new(Context) AttrTy(Context, AL, AL.getTCBName());
}

EnforceTCBAttr *Sema::mergeEnforceTCBAttr(Decl *D, const EnforceTCBAttr &AL) {
  return mergeEnforceTCBAttrImpl<EnforceTCBAttr, EnforceTCBLeafAttr>(
      *this, D, AL);
}

EnforceTCBLeafAttr *Sema::mergeEnforceTCBLeafAttr(
    Decl *D, const EnforceTCBLeafAttr &AL) {
  return mergeEnforceTCBAttrImpl<EnforceTCBLeafAttr, EnforceTCBAttr>(
      *this, D, AL);
}

static void handleVTablePointerAuthentication(Sema &S, Decl *D,
                                              const ParsedAttr &AL) {
  CXXRecordDecl *Decl = cast<CXXRecordDecl>(D);
  const uint32_t NumArgs = AL.getNumArgs();
  if (NumArgs > 4) {
    S.Diag(AL.getLoc(), diag::err_attribute_too_many_arguments) << AL << 4;
    AL.setInvalid();
  }

  if (NumArgs == 0) {
    S.Diag(AL.getLoc(), diag::err_attribute_too_few_arguments) << AL;
    AL.setInvalid();
    return;
  }

  if (D->getAttr<VTablePointerAuthenticationAttr>()) {
    S.Diag(AL.getLoc(), diag::err_duplicated_vtable_pointer_auth) << Decl;
    AL.setInvalid();
  }

  auto KeyType = VTablePointerAuthenticationAttr::VPtrAuthKeyType::DefaultKey;
  if (AL.isArgIdent(0)) {
    IdentifierLoc *IL = AL.getArgAsIdent(0);
    if (!VTablePointerAuthenticationAttr::ConvertStrToVPtrAuthKeyType(
            IL->getIdentifierInfo()->getName(), KeyType)) {
      S.Diag(IL->getLoc(), diag::err_invalid_authentication_key)
          << IL->getIdentifierInfo();
      AL.setInvalid();
    }
    if (KeyType == VTablePointerAuthenticationAttr::DefaultKey &&
        !S.getLangOpts().PointerAuthCalls) {
      S.Diag(AL.getLoc(), diag::err_no_default_vtable_pointer_auth) << 0;
      AL.setInvalid();
    }
  } else {
    S.Diag(AL.getLoc(), diag::err_attribute_argument_type)
        << AL << AANT_ArgumentIdentifier;
    return;
  }

  auto AddressDiversityMode = VTablePointerAuthenticationAttr::
      AddressDiscriminationMode::DefaultAddressDiscrimination;
  if (AL.getNumArgs() > 1) {
    if (AL.isArgIdent(1)) {
      IdentifierLoc *IL = AL.getArgAsIdent(1);
      if (!VTablePointerAuthenticationAttr::
              ConvertStrToAddressDiscriminationMode(
                  IL->getIdentifierInfo()->getName(), AddressDiversityMode)) {
        S.Diag(IL->getLoc(), diag::err_invalid_address_discrimination)
            << IL->getIdentifierInfo();
        AL.setInvalid();
      }
      if (AddressDiversityMode ==
              VTablePointerAuthenticationAttr::DefaultAddressDiscrimination &&
          !S.getLangOpts().PointerAuthCalls) {
        S.Diag(IL->getLoc(), diag::err_no_default_vtable_pointer_auth) << 1;
        AL.setInvalid();
      }
    } else {
      S.Diag(AL.getLoc(), diag::err_attribute_argument_type)
          << AL << AANT_ArgumentIdentifier;
    }
  }

  auto ED = VTablePointerAuthenticationAttr::ExtraDiscrimination::
      DefaultExtraDiscrimination;
  if (AL.getNumArgs() > 2) {
    if (AL.isArgIdent(2)) {
      IdentifierLoc *IL = AL.getArgAsIdent(2);
      if (!VTablePointerAuthenticationAttr::ConvertStrToExtraDiscrimination(
              IL->getIdentifierInfo()->getName(), ED)) {
        S.Diag(IL->getLoc(), diag::err_invalid_extra_discrimination)
            << IL->getIdentifierInfo();
        AL.setInvalid();
      }
      if (ED == VTablePointerAuthenticationAttr::DefaultExtraDiscrimination &&
          !S.getLangOpts().PointerAuthCalls) {
        S.Diag(AL.getLoc(), diag::err_no_default_vtable_pointer_auth) << 2;
        AL.setInvalid();
      }
    } else {
      S.Diag(AL.getLoc(), diag::err_attribute_argument_type)
          << AL << AANT_ArgumentIdentifier;
    }
  }

  uint32_t CustomDiscriminationValue = 0;
  if (ED == VTablePointerAuthenticationAttr::CustomDiscrimination) {
    if (NumArgs < 4) {
      S.Diag(AL.getLoc(), diag::err_missing_custom_discrimination) << AL << 4;
      AL.setInvalid();
      return;
    }
    if (NumArgs > 4) {
      S.Diag(AL.getLoc(), diag::err_attribute_too_many_arguments) << AL << 4;
      AL.setInvalid();
    }

    if (!AL.isArgExpr(3) || !S.checkUInt32Argument(AL, AL.getArgAsExpr(3),
                                                   CustomDiscriminationValue)) {
      S.Diag(AL.getLoc(), diag::err_invalid_custom_discrimination);
      AL.setInvalid();
    }
  } else if (NumArgs > 3) {
    S.Diag(AL.getLoc(), diag::err_attribute_too_many_arguments) << AL << 3;
    AL.setInvalid();
  }

  Decl->addAttr(::new (S.Context) VTablePointerAuthenticationAttr(
      S.Context, AL, KeyType, AddressDiversityMode, ED,
      CustomDiscriminationValue));
}

//===----------------------------------------------------------------------===//
// Top Level Sema Entry Points
//===----------------------------------------------------------------------===//

// Returns true if the attribute must delay setting its arguments until after
// template instantiation, and false otherwise.
static bool MustDelayAttributeArguments(const ParsedAttr &AL) {
  // Only attributes that accept expression parameter packs can delay arguments.
  if (!AL.acceptsExprPack())
    return false;

  bool AttrHasVariadicArg = AL.hasVariadicArg();
  unsigned AttrNumArgs = AL.getNumArgMembers();
  for (size_t I = 0; I < std::min(AL.getNumArgs(), AttrNumArgs); ++I) {
    bool IsLastAttrArg = I == (AttrNumArgs - 1);
    // If the argument is the last argument and it is variadic it can contain
    // any expression.
    if (IsLastAttrArg && AttrHasVariadicArg)
      return false;
    Expr *E = AL.getArgAsExpr(I);
    bool ArgMemberCanHoldExpr = AL.isParamExpr(I);
    // If the expression is a pack expansion then arguments must be delayed
    // unless the argument is an expression and it is the last argument of the
    // attribute.
    if (isa<PackExpansionExpr>(E))
      return !(IsLastAttrArg && ArgMemberCanHoldExpr);
    // Last case is if the expression is value dependent then it must delay
    // arguments unless the corresponding argument is able to hold the
    // expression.
    if (E->isValueDependent() && !ArgMemberCanHoldExpr)
      return true;
  }
  return false;
}

/// ProcessDeclAttribute - Apply the specific attribute to the specified decl if
/// the attribute applies to decls.  If the attribute is a type attribute, just
/// silently ignore it if a GNU attribute.
static void
ProcessDeclAttribute(Sema &S, Scope *scope, Decl *D, const ParsedAttr &AL,
                     const Sema::ProcessDeclAttributeOptions &Options) {
  if (AL.isInvalid() || AL.getKind() == ParsedAttr::IgnoredAttribute)
    return;

  // Ignore C++11 attributes on declarator chunks: they appertain to the type
  // instead. Note, isCXX11Attribute() will look at whether the attribute is
  // [[]] or alignas, while isC23Attribute() will only look at [[]]. This is
  // important for ensuring that alignas in C23 is properly handled on a
  // structure member declaration because it is a type-specifier-qualifier in
  // C but still applies to the declaration rather than the type.
  if ((S.getLangOpts().CPlusPlus ? AL.isCXX11Attribute()
                                 : AL.isC23Attribute()) &&
      !Options.IncludeCXX11Attributes)
    return;

  // Unknown attributes are automatically warned on. Target-specific attributes
  // which do not apply to the current target architecture are treated as
  // though they were unknown attributes.
  if (AL.getKind() == ParsedAttr::UnknownAttribute ||
      !AL.existsInTarget(S.Context.getTargetInfo())) {
    if (AL.isRegularKeywordAttribute() || AL.isDeclspecAttribute()) {
      S.Diag(AL.getLoc(), AL.isRegularKeywordAttribute()
                              ? diag::err_keyword_not_supported_on_target
                              : diag::warn_unhandled_ms_attribute_ignored)
          << AL.getAttrName() << AL.getRange();
    } else {
      S.DiagnoseUnknownAttribute(AL);
    }
    return;
  }

  // Check if argument population must delayed to after template instantiation.
  bool MustDelayArgs = MustDelayAttributeArguments(AL);

  // Argument number check must be skipped if arguments are delayed.
  if (S.checkCommonAttributeFeatures(D, AL, MustDelayArgs))
    return;

  if (MustDelayArgs) {
    AL.handleAttrWithDelayedArgs(S, D);
    return;
  }

  switch (AL.getKind()) {
  default:
    if (AL.getInfo().handleDeclAttribute(S, D, AL) != ParsedAttrInfo::NotHandled)
      break;
    if (!AL.isStmtAttr()) {
      assert(AL.isTypeAttr() && "Non-type attribute not handled");
    }
    if (AL.isTypeAttr()) {
      if (Options.IgnoreTypeAttributes)
        break;
      if (!AL.isStandardAttributeSyntax() && !AL.isRegularKeywordAttribute()) {
        // Non-[[]] type attributes are handled in processTypeAttrs(); silently
        // move on.
        break;
      }

      // According to the C and C++ standards, we should never see a
      // [[]] type attribute on a declaration. However, we have in the past
      // allowed some type attributes to "slide" to the `DeclSpec`, so we need
      // to continue to support this legacy behavior. We only do this, however,
      // if
      // - we actually have a `DeclSpec`, i.e. if we're looking at a
      //   `DeclaratorDecl`, or
      // - we are looking at an alias-declaration, where historically we have
      //   allowed type attributes after the identifier to slide to the type.
      if (AL.slidesFromDeclToDeclSpecLegacyBehavior() &&
          isa<DeclaratorDecl, TypeAliasDecl>(D)) {
        // Suggest moving the attribute to the type instead, but only for our
        // own vendor attributes; moving other vendors' attributes might hurt
        // portability.
        if (AL.isClangScope()) {
          S.Diag(AL.getLoc(), diag::warn_type_attribute_deprecated_on_decl)
              << AL << D->getLocation();
        }

        // Allow this type attribute to be handled in processTypeAttrs();
        // silently move on.
        break;
      }

      if (AL.getKind() == ParsedAttr::AT_Regparm) {
        // `regparm` is a special case: It's a type attribute but we still want
        // to treat it as if it had been written on the declaration because that
        // way we'll be able to handle it directly in `processTypeAttr()`.
        // If we treated `regparm` it as if it had been written on the
        // `DeclSpec`, the logic in `distributeFunctionTypeAttrFromDeclSepc()`
        // would try to move it to the declarator, but that doesn't work: We
        // can't remove the attribute from the list of declaration attributes
        // because it might be needed by other declarators in the same
        // declaration.
        break;
      }

      if (AL.getKind() == ParsedAttr::AT_VectorSize) {
        // `vector_size` is a special case: It's a type attribute semantically,
        // but GCC expects the [[]] syntax to be written on the declaration (and
        // warns that the attribute has no effect if it is placed on the
        // decl-specifier-seq).
        // Silently move on and allow the attribute to be handled in
        // processTypeAttr().
        break;
      }

      if (AL.getKind() == ParsedAttr::AT_NoDeref) {
        // FIXME: `noderef` currently doesn't work correctly in [[]] syntax.
        // See https://github.com/llvm/llvm-project/issues/55790 for details.
        // We allow processTypeAttrs() to emit a warning and silently move on.
        break;
      }
    }
    // N.B., ClangAttrEmitter.cpp emits a diagnostic helper that ensures a
    // statement attribute is not written on a declaration, but this code is
    // needed for type attributes as well as statement attributes in Attr.td
    // that do not list any subjects.
    S.Diag(AL.getLoc(), diag::err_attribute_invalid_on_decl)
        << AL << AL.isRegularKeywordAttribute() << D->getLocation();
    break;
  case ParsedAttr::AT_Interrupt:
    handleInterruptAttr(S, D, AL);
    break;
  case ParsedAttr::AT_ARMInterruptSaveFP:
    S.ARM().handleInterruptSaveFPAttr(D, AL);
    break;
  case ParsedAttr::AT_X86ForceAlignArgPointer:
    S.X86().handleForceAlignArgPointerAttr(D, AL);
    break;
  case ParsedAttr::AT_ReadOnlyPlacement:
    handleSimpleAttribute<ReadOnlyPlacementAttr>(S, D, AL);
    break;
  case ParsedAttr::AT_DLLExport:
  case ParsedAttr::AT_DLLImport:
    handleDLLAttr(S, D, AL);
    break;
  case ParsedAttr::AT_AMDGPUFlatWorkGroupSize:
    S.AMDGPU().handleAMDGPUFlatWorkGroupSizeAttr(D, AL);
    break;
  case ParsedAttr::AT_AMDGPUWavesPerEU:
    S.AMDGPU().handleAMDGPUWavesPerEUAttr(D, AL);
    break;
  case ParsedAttr::AT_AMDGPUNumSGPR:
    S.AMDGPU().handleAMDGPUNumSGPRAttr(D, AL);
    break;
  case ParsedAttr::AT_AMDGPUNumVGPR:
    S.AMDGPU().handleAMDGPUNumVGPRAttr(D, AL);
    break;
  case ParsedAttr::AT_AMDGPUMaxNumWorkGroups:
    S.AMDGPU().handleAMDGPUMaxNumWorkGroupsAttr(D, AL);
    break;
  case ParsedAttr::AT_AVRSignal:
    S.AVR().handleSignalAttr(D, AL);
    break;
  case ParsedAttr::AT_BPFPreserveAccessIndex:
    S.BPF().handlePreserveAccessIndexAttr(D, AL);
    break;
  case ParsedAttr::AT_BPFPreserveStaticOffset:
    handleSimpleAttribute<BPFPreserveStaticOffsetAttr>(S, D, AL);
    break;
  case ParsedAttr::AT_BTFDeclTag:
    handleBTFDeclTagAttr(S, D, AL);
    break;
  case ParsedAttr::AT_WebAssemblyExportName:
    S.Wasm().handleWebAssemblyExportNameAttr(D, AL);
    break;
  case ParsedAttr::AT_WebAssemblyImportModule:
    S.Wasm().handleWebAssemblyImportModuleAttr(D, AL);
    break;
  case ParsedAttr::AT_WebAssemblyImportName:
    S.Wasm().handleWebAssemblyImportNameAttr(D, AL);
    break;
  case ParsedAttr::AT_IBOutlet:
    S.ObjC().handleIBOutlet(D, AL);
    break;
  case ParsedAttr::AT_IBOutletCollection:
    S.ObjC().handleIBOutletCollection(D, AL);
    break;
  case ParsedAttr::AT_IFunc:
    handleIFuncAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Alias:
    handleAliasAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Aligned:
    handleAlignedAttr(S, D, AL);
    break;
  case ParsedAttr::AT_AlignValue:
    handleAlignValueAttr(S, D, AL);
    break;
  case ParsedAttr::AT_AllocSize:
    handleAllocSizeAttr(S, D, AL);
    break;
  case ParsedAttr::AT_AlwaysInline:
    handleAlwaysInlineAttr(S, D, AL);
    break;
  case ParsedAttr::AT_AnalyzerNoReturn:
    handleAnalyzerNoReturnAttr(S, D, AL);
    break;
  case ParsedAttr::AT_TLSModel:
    handleTLSModelAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Annotate:
    handleAnnotateAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Availability:
    handleAvailabilityAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CarriesDependency:
    handleDependencyAttr(S, scope, D, AL);
    break;
  case ParsedAttr::AT_CPUDispatch:
  case ParsedAttr::AT_CPUSpecific:
    handleCPUSpecificAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Common:
    handleCommonAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CUDAConstant:
    handleConstantAttr(S, D, AL);
    break;
  case ParsedAttr::AT_PassObjectSize:
    handlePassObjectSizeAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Constructor:
      handleConstructorAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Deprecated:
    handleDeprecatedAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Destructor:
      handleDestructorAttr(S, D, AL);
    break;
  case ParsedAttr::AT_EnableIf:
    handleEnableIfAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Error:
    handleErrorAttr(S, D, AL);
    break;
  case ParsedAttr::AT_ExcludeFromExplicitInstantiation:
    handleExcludeFromExplicitInstantiationAttr(S, D, AL);
    break;
  case ParsedAttr::AT_DiagnoseIf:
    handleDiagnoseIfAttr(S, D, AL);
    break;
  case ParsedAttr::AT_DiagnoseAsBuiltin:
    handleDiagnoseAsBuiltinAttr(S, D, AL);
    break;
  case ParsedAttr::AT_NoBuiltin:
    handleNoBuiltinAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CFIUncheckedCallee:
    handleCFIUncheckedCalleeAttr(S, D, AL);
    break;
  case ParsedAttr::AT_ExtVectorType:
    handleExtVectorTypeAttr(S, D, AL);
    break;
  case ParsedAttr::AT_ExternalSourceSymbol:
    handleExternalSourceSymbolAttr(S, D, AL);
    break;
  case ParsedAttr::AT_MinSize:
    handleMinSizeAttr(S, D, AL);
    break;
  case ParsedAttr::AT_OptimizeNone:
    handleOptimizeNoneAttr(S, D, AL);
    break;
  case ParsedAttr::AT_EnumExtensibility:
    handleEnumExtensibilityAttr(S, D, AL);
    break;
  case ParsedAttr::AT_SYCLKernelEntryPoint:
    S.SYCL().handleKernelEntryPointAttr(D, AL);
    break;
  case ParsedAttr::AT_SYCLSpecialClass:
    handleSimpleAttribute<SYCLSpecialClassAttr>(S, D, AL);
    break;
  case ParsedAttr::AT_Format:
    handleFormatAttr(S, D, AL);
    break;
  case ParsedAttr::AT_FormatMatches:
    handleFormatMatchesAttr(S, D, AL);
    break;
  case ParsedAttr::AT_FormatArg:
    handleFormatArgAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Callback:
    handleCallbackAttr(S, D, AL);
    break;
  case ParsedAttr::AT_LifetimeCaptureBy:
    handleLifetimeCaptureByAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CalledOnce:
    handleCalledOnceAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CUDAGlobal:
    handleGlobalAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CUDADevice:
    handleDeviceAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CUDAGridConstant:
    handleGridConstantAttr(S, D, AL);
    break;
  case ParsedAttr::AT_HIPManaged:
    handleManagedAttr(S, D, AL);
    break;
  case ParsedAttr::AT_GNUInline:
    handleGNUInlineAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CUDALaunchBounds:
    handleLaunchBoundsAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Restrict:
    handleRestrictAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Mode:
    handleModeAttr(S, D, AL);
    break;
  case ParsedAttr::AT_NonString:
    handleNonStringAttr(S, D, AL);
    break;
  case ParsedAttr::AT_NonNull:
    if (auto *PVD = dyn_cast<ParmVarDecl>(D))
      handleNonNullAttrParameter(S, PVD, AL);
    else
      handleNonNullAttr(S, D, AL);
    break;
  case ParsedAttr::AT_ReturnsNonNull:
    handleReturnsNonNullAttr(S, D, AL);
    break;
  case ParsedAttr::AT_NoEscape:
    handleNoEscapeAttr(S, D, AL);
    break;
  case ParsedAttr::AT_MaybeUndef:
    handleSimpleAttribute<MaybeUndefAttr>(S, D, AL);
    break;
  case ParsedAttr::AT_AssumeAligned:
    handleAssumeAlignedAttr(S, D, AL);
    break;
  case ParsedAttr::AT_AllocAlign:
    handleAllocAlignAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Ownership:
    handleOwnershipAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Naked:
    handleNakedAttr(S, D, AL);
    break;
  case ParsedAttr::AT_NoReturn:
    handleNoReturnAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CXX11NoReturn:
    handleStandardNoReturnAttr(S, D, AL);
    break;
  case ParsedAttr::AT_AnyX86NoCfCheck:
    handleNoCfCheckAttr(S, D, AL);
    break;
  case ParsedAttr::AT_NoThrow:
    if (!AL.isUsedAsTypeAttr())
      handleSimpleAttribute<NoThrowAttr>(S, D, AL);
    break;
  case ParsedAttr::AT_CUDAShared:
    handleSharedAttr(S, D, AL);
    break;
  case ParsedAttr::AT_VecReturn:
    handleVecReturnAttr(S, D, AL);
    break;
  case ParsedAttr::AT_ObjCOwnership:
    S.ObjC().handleOwnershipAttr(D, AL);
    break;
  case ParsedAttr::AT_ObjCPreciseLifetime:
    S.ObjC().handlePreciseLifetimeAttr(D, AL);
    break;
  case ParsedAttr::AT_ObjCReturnsInnerPointer:
    S.ObjC().handleReturnsInnerPointerAttr(D, AL);
    break;
  case ParsedAttr::AT_ObjCRequiresSuper:
    S.ObjC().handleRequiresSuperAttr(D, AL);
    break;
  case ParsedAttr::AT_ObjCBridge:
    S.ObjC().handleBridgeAttr(D, AL);
    break;
  case ParsedAttr::AT_ObjCBridgeMutable:
    S.ObjC().handleBridgeMutableAttr(D, AL);
    break;
  case ParsedAttr::AT_ObjCBridgeRelated:
    S.ObjC().handleBridgeRelatedAttr(D, AL);
    break;
  case ParsedAttr::AT_ObjCDesignatedInitializer:
    S.ObjC().handleDesignatedInitializer(D, AL);
    break;
  case ParsedAttr::AT_ObjCRuntimeName:
    S.ObjC().handleRuntimeName(D, AL);
    break;
  case ParsedAttr::AT_ObjCBoxable:
    S.ObjC().handleBoxable(D, AL);
    break;
  case ParsedAttr::AT_NSErrorDomain:
    S.ObjC().handleNSErrorDomain(D, AL);
    break;
  case ParsedAttr::AT_CFConsumed:
  case ParsedAttr::AT_NSConsumed:
  case ParsedAttr::AT_OSConsumed:
    S.ObjC().AddXConsumedAttr(D, AL,
                              S.ObjC().parsedAttrToRetainOwnershipKind(AL),
                              /*IsTemplateInstantiation=*/false);
    break;
  case ParsedAttr::AT_OSReturnsRetainedOnZero:
    handleSimpleAttributeOrDiagnose<OSReturnsRetainedOnZeroAttr>(
        S, D, AL, S.ObjC().isValidOSObjectOutParameter(D),
        diag::warn_ns_attribute_wrong_parameter_type,
        /*Extra Args=*/AL, /*pointer-to-OSObject-pointer*/ 3, AL.getRange());
    break;
  case ParsedAttr::AT_OSReturnsRetainedOnNonZero:
    handleSimpleAttributeOrDiagnose<OSReturnsRetainedOnNonZeroAttr>(
        S, D, AL, S.ObjC().isValidOSObjectOutParameter(D),
        diag::warn_ns_attribute_wrong_parameter_type,
        /*Extra Args=*/AL, /*pointer-to-OSObject-poointer*/ 3, AL.getRange());
    break;
  case ParsedAttr::AT_NSReturnsAutoreleased:
  case ParsedAttr::AT_NSReturnsNotRetained:
  case ParsedAttr::AT_NSReturnsRetained:
  case ParsedAttr::AT_CFReturnsNotRetained:
  case ParsedAttr::AT_CFReturnsRetained:
  case ParsedAttr::AT_OSReturnsNotRetained:
  case ParsedAttr::AT_OSReturnsRetained:
    S.ObjC().handleXReturnsXRetainedAttr(D, AL);
    break;
  case ParsedAttr::AT_WorkGroupSizeHint:
    handleWorkGroupSize<WorkGroupSizeHintAttr>(S, D, AL);
    break;
  case ParsedAttr::AT_ReqdWorkGroupSize:
    handleWorkGroupSize<ReqdWorkGroupSizeAttr>(S, D, AL);
    break;
  case ParsedAttr::AT_OpenCLIntelReqdSubGroupSize:
    S.OpenCL().handleSubGroupSize(D, AL);
    break;
  case ParsedAttr::AT_VecTypeHint:
    handleVecTypeHint(S, D, AL);
    break;
  case ParsedAttr::AT_InitPriority:
      handleInitPriorityAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Packed:
    handlePackedAttr(S, D, AL);
    break;
  case ParsedAttr::AT_PreferredName:
    handlePreferredName(S, D, AL);
    break;
  case ParsedAttr::AT_NoSpecializations:
    handleNoSpecializations(S, D, AL);
    break;
  case ParsedAttr::AT_Section:
    handleSectionAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CodeModel:
    handleCodeModelAttr(S, D, AL);
    break;
  case ParsedAttr::AT_RandomizeLayout:
    handleRandomizeLayoutAttr(S, D, AL);
    break;
  case ParsedAttr::AT_NoRandomizeLayout:
    handleNoRandomizeLayoutAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CodeSeg:
    handleCodeSegAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Target:
    handleTargetAttr(S, D, AL);
    break;
  case ParsedAttr::AT_TargetVersion:
    handleTargetVersionAttr(S, D, AL);
    break;
  case ParsedAttr::AT_TargetClones:
    handleTargetClonesAttr(S, D, AL);
    break;
  case ParsedAttr::AT_MinVectorWidth:
    handleMinVectorWidthAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Unavailable:
    handleAttrWithMessage<UnavailableAttr>(S, D, AL);
    break;
  case ParsedAttr::AT_OMPAssume:
    S.OpenMP().handleOMPAssumeAttr(D, AL);
    break;
  case ParsedAttr::AT_ObjCDirect:
    S.ObjC().handleDirectAttr(D, AL);
    break;
  case ParsedAttr::AT_ObjCDirectMembers:
    S.ObjC().handleDirectMembersAttr(D, AL);
    handleSimpleAttribute<ObjCDirectMembersAttr>(S, D, AL);
    break;
  case ParsedAttr::AT_ObjCExplicitProtocolImpl:
    S.ObjC().handleSuppresProtocolAttr(D, AL);
    break;
  case ParsedAttr::AT_Unused:
    handleUnusedAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Visibility:
    handleVisibilityAttr(S, D, AL, false);
    break;
  case ParsedAttr::AT_TypeVisibility:
    handleVisibilityAttr(S, D, AL, true);
    break;
  case ParsedAttr::AT_WarnUnusedResult:
    handleWarnUnusedResult(S, D, AL);
    break;
  case ParsedAttr::AT_WeakRef:
    handleWeakRefAttr(S, D, AL);
    break;
  case ParsedAttr::AT_WeakImport:
    handleWeakImportAttr(S, D, AL);
    break;
  case ParsedAttr::AT_TransparentUnion:
    handleTransparentUnionAttr(S, D, AL);
    break;
  case ParsedAttr::AT_ObjCMethodFamily:
    S.ObjC().handleMethodFamilyAttr(D, AL);
    break;
  case ParsedAttr::AT_ObjCNSObject:
    S.ObjC().handleNSObject(D, AL);
    break;
  case ParsedAttr::AT_ObjCIndependentClass:
    S.ObjC().handleIndependentClass(D, AL);
    break;
  case ParsedAttr::AT_Blocks:
    S.ObjC().handleBlocksAttr(D, AL);
    break;
  case ParsedAttr::AT_Sentinel:
    handleSentinelAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Cleanup:
    handleCleanupAttr(S, D, AL);
    break;
  case ParsedAttr::AT_NoDebug:
    handleNoDebugAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CmseNSEntry:
    S.ARM().handleCmseNSEntryAttr(D, AL);
    break;
  case ParsedAttr::AT_StdCall:
  case ParsedAttr::AT_CDecl:
  case ParsedAttr::AT_FastCall:
  case ParsedAttr::AT_ThisCall:
  case ParsedAttr::AT_Pascal:
  case ParsedAttr::AT_RegCall:
  case ParsedAttr::AT_SwiftCall:
  case ParsedAttr::AT_SwiftAsyncCall:
  case ParsedAttr::AT_VectorCall:
  case ParsedAttr::AT_MSABI:
  case ParsedAttr::AT_SysVABI:
  case ParsedAttr::AT_Pcs:
  case ParsedAttr::AT_IntelOclBicc:
  case ParsedAttr::AT_PreserveMost:
  case ParsedAttr::AT_PreserveAll:
  case ParsedAttr::AT_AArch64VectorPcs:
  case ParsedAttr::AT_AArch64SVEPcs:
  case ParsedAttr::AT_M68kRTD:
  case ParsedAttr::AT_PreserveNone:
  case ParsedAttr::AT_RISCVVectorCC:
  case ParsedAttr::AT_RISCVVLSCC:
    handleCallConvAttr(S, D, AL);
    break;
  case ParsedAttr::AT_DeviceKernel:
    handleDeviceKernelAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Suppress:
    handleSuppressAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Owner:
  case ParsedAttr::AT_Pointer:
    handleLifetimeCategoryAttr(S, D, AL);
    break;
  case ParsedAttr::AT_OpenCLAccess:
    S.OpenCL().handleAccessAttr(D, AL);
    break;
  case ParsedAttr::AT_OpenCLNoSVM:
    S.OpenCL().handleNoSVMAttr(D, AL);
    break;
  case ParsedAttr::AT_SwiftContext:
    S.Swift().AddParameterABIAttr(D, AL, ParameterABI::SwiftContext);
    break;
  case ParsedAttr::AT_SwiftAsyncContext:
    S.Swift().AddParameterABIAttr(D, AL, ParameterABI::SwiftAsyncContext);
    break;
  case ParsedAttr::AT_SwiftErrorResult:
    S.Swift().AddParameterABIAttr(D, AL, ParameterABI::SwiftErrorResult);
    break;
  case ParsedAttr::AT_SwiftIndirectResult:
    S.Swift().AddParameterABIAttr(D, AL, ParameterABI::SwiftIndirectResult);
    break;
  case ParsedAttr::AT_InternalLinkage:
    handleInternalLinkageAttr(S, D, AL);
    break;
  case ParsedAttr::AT_ZeroCallUsedRegs:
    handleZeroCallUsedRegsAttr(S, D, AL);
    break;
  case ParsedAttr::AT_FunctionReturnThunks:
    handleFunctionReturnThunksAttr(S, D, AL);
    break;
  case ParsedAttr::AT_NoMerge:
    handleNoMergeAttr(S, D, AL);
    break;
  case ParsedAttr::AT_NoUniqueAddress:
    handleNoUniqueAddressAttr(S, D, AL);
    break;

  case ParsedAttr::AT_AvailableOnlyInDefaultEvalMethod:
    handleAvailableOnlyInDefaultEvalMethod(S, D, AL);
    break;

  case ParsedAttr::AT_CountedBy:
  case ParsedAttr::AT_CountedByOrNull:
  case ParsedAttr::AT_SizedBy:
  case ParsedAttr::AT_SizedByOrNull:
    handleCountedByAttrField(S, D, AL);
    break;

  // Microsoft attributes:
  case ParsedAttr::AT_LayoutVersion:
    handleLayoutVersion(S, D, AL);
    break;
  case ParsedAttr::AT_Uuid:
    handleUuidAttr(S, D, AL);
    break;
  case ParsedAttr::AT_MSInheritance:
    handleMSInheritanceAttr(S, D, AL);
    break;
  case ParsedAttr::AT_Thread:
    handleDeclspecThreadAttr(S, D, AL);
    break;
  case ParsedAttr::AT_MSConstexpr:
    handleMSConstexprAttr(S, D, AL);
    break;
  case ParsedAttr::AT_HybridPatchable:
    handleSimpleAttribute<HybridPatchableAttr>(S, D, AL);
    break;

  // HLSL attributes:
  case ParsedAttr::AT_RootSignature:
    S.HLSL().handleRootSignatureAttr(D, AL);
    break;
  case ParsedAttr::AT_HLSLNumThreads:
    S.HLSL().handleNumThreadsAttr(D, AL);
    break;
  case ParsedAttr::AT_HLSLWaveSize:
    S.HLSL().handleWaveSizeAttr(D, AL);
    break;
  case ParsedAttr::AT_HLSLSV_Position:
    S.HLSL().handleSV_PositionAttr(D, AL);
    break;
  case ParsedAttr::AT_HLSLVkExtBuiltinInput:
    S.HLSL().handleVkExtBuiltinInputAttr(D, AL);
    break;
  case ParsedAttr::AT_HLSLVkConstantId:
    S.HLSL().handleVkConstantIdAttr(D, AL);
    break;
  case ParsedAttr::AT_HLSLSV_GroupThreadID:
    S.HLSL().handleSV_GroupThreadIDAttr(D, AL);
    break;
  case ParsedAttr::AT_HLSLSV_GroupID:
    S.HLSL().handleSV_GroupIDAttr(D, AL);
    break;
  case ParsedAttr::AT_HLSLSV_GroupIndex:
    handleSimpleAttribute<HLSLSV_GroupIndexAttr>(S, D, AL);
    break;
  case ParsedAttr::AT_HLSLGroupSharedAddressSpace:
    handleSimpleAttribute<HLSLGroupSharedAddressSpaceAttr>(S, D, AL);
    break;
  case ParsedAttr::AT_HLSLSV_DispatchThreadID:
    S.HLSL().handleSV_DispatchThreadIDAttr(D, AL);
    break;
  case ParsedAttr::AT_HLSLPackOffset:
    S.HLSL().handlePackOffsetAttr(D, AL);
    break;
  case ParsedAttr::AT_HLSLShader:
    S.HLSL().handleShaderAttr(D, AL);
    break;
  case ParsedAttr::AT_HLSLResourceBinding:
    S.HLSL().handleResourceBindingAttr(D, AL);
    break;
  case ParsedAttr::AT_HLSLParamModifier:
    S.HLSL().handleParamModifierAttr(D, AL);
    break;

  case ParsedAttr::AT_AbiTag:
    handleAbiTagAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CFGuard:
    handleCFGuardAttr(S, D, AL);
    break;

  // Thread safety attributes:
  case ParsedAttr::AT_PtGuardedVar:
    handlePtGuardedVarAttr(S, D, AL);
    break;
  case ParsedAttr::AT_NoSanitize:
    handleNoSanitizeAttr(S, D, AL);
    break;
  case ParsedAttr::AT_NoSanitizeSpecific:
    handleNoSanitizeSpecificAttr(S, D, AL);
    break;
  case ParsedAttr::AT_GuardedBy:
    handleGuardedByAttr(S, D, AL);
    break;
  case ParsedAttr::AT_PtGuardedBy:
    handlePtGuardedByAttr(S, D, AL);
    break;
  case ParsedAttr::AT_LockReturned:
    handleLockReturnedAttr(S, D, AL);
    break;
  case ParsedAttr::AT_LocksExcluded:
    handleLocksExcludedAttr(S, D, AL);
    break;
  case ParsedAttr::AT_AcquiredBefore:
    handleAcquiredBeforeAttr(S, D, AL);
    break;
  case ParsedAttr::AT_AcquiredAfter:
    handleAcquiredAfterAttr(S, D, AL);
    break;

  // Capability analysis attributes.
  case ParsedAttr::AT_Capability:
  case ParsedAttr::AT_Lockable:
    handleCapabilityAttr(S, D, AL);
    break;
  case ParsedAttr::AT_ReentrantCapability:
    handleReentrantCapabilityAttr(S, D, AL);
    break;
  case ParsedAttr::AT_RequiresCapability:
    handleRequiresCapabilityAttr(S, D, AL);
    break;

  case ParsedAttr::AT_AssertCapability:
    handleAssertCapabilityAttr(S, D, AL);
    break;
  case ParsedAttr::AT_AcquireCapability:
    handleAcquireCapabilityAttr(S, D, AL);
    break;
  case ParsedAttr::AT_ReleaseCapability:
    handleReleaseCapabilityAttr(S, D, AL);
    break;
  case ParsedAttr::AT_TryAcquireCapability:
    handleTryAcquireCapabilityAttr(S, D, AL);
    break;

  // Consumed analysis attributes.
  case ParsedAttr::AT_Consumable:
    handleConsumableAttr(S, D, AL);
    break;
  case ParsedAttr::AT_CallableWhen:
    handleCallableWhenAttr(S, D, AL);
    break;
  case ParsedAttr::AT_ParamTypestate:
    handleParamTypestateAttr(S, D, AL);
    break;
  case ParsedAttr::AT_ReturnTypestate:
    handleReturnTypestateAttr(S, D, AL);
    break;
  case ParsedAttr::AT_SetTypestate:
    handleSetTypestateAttr(S, D, AL);
    break;
  case ParsedAttr::AT_TestTypestate:
    handleTestTypestateAttr(S, D, AL);
    break;

  // Type safety attributes.
  case ParsedAttr::AT_ArgumentWithTypeTag:
    handleArgumentWithTypeTagAttr(S, D, AL);
    break;
  case ParsedAttr::AT_TypeTagForDatatype:
    handleTypeTagForDatatypeAttr(S, D, AL);
    break;

  // Swift attributes.
  case ParsedAttr::AT_SwiftAsyncName:
    S.Swift().handleAsyncName(D, AL);
    break;
  case ParsedAttr::AT_SwiftAttr:
    S.Swift().handleAttrAttr(D, AL);
    break;
  case ParsedAttr::AT_SwiftBridge:
    S.Swift().handleBridge(D, AL);
    break;
  case ParsedAttr::AT_SwiftError:
    S.Swift().handleError(D, AL);
    break;
  case ParsedAttr::AT_SwiftName:
    S.Swift().handleName(D, AL);
    break;
  case ParsedAttr::AT_SwiftNewType:
    S.Swift().handleNewType(D, AL);
    break;
  case ParsedAttr::AT_SwiftAsync:
    S.Swift().handleAsyncAttr(D, AL);
    break;
  case ParsedAttr::AT_SwiftAsyncError:
    S.Swift().handleAsyncError(D, AL);
    break;

  // XRay attributes.
  case ParsedAttr::AT_XRayLogArgs:
    handleXRayLogArgsAttr(S, D, AL);
    break;

  case ParsedAttr::AT_PatchableFunctionEntry:
    handlePatchableFunctionEntryAttr(S, D, AL);
    break;

  case ParsedAttr::AT_AlwaysDestroy:
  case ParsedAttr::AT_NoDestroy:
    handleDestroyAttr(S, D, AL);
    break;

  case ParsedAttr::AT_Uninitialized:
    handleUninitializedAttr(S, D, AL);
    break;

  case ParsedAttr::AT_ObjCExternallyRetained:
    S.ObjC().handleExternallyRetainedAttr(D, AL);
    break;

  case ParsedAttr::AT_MIGServerRoutine:
    handleMIGServerRoutineAttr(S, D, AL);
    break;

  case ParsedAttr::AT_MSAllocator:
    handleMSAllocatorAttr(S, D, AL);
    break;

  case ParsedAttr::AT_ArmBuiltinAlias:
    S.ARM().handleBuiltinAliasAttr(D, AL);
    break;

  case ParsedAttr::AT_ArmLocallyStreaming:
    handleSimpleAttribute<ArmLocallyStreamingAttr>(S, D, AL);
    break;

  case ParsedAttr::AT_ArmNew:
    S.ARM().handleNewAttr(D, AL);
    break;

  case ParsedAttr::AT_AcquireHandle:
    handleAcquireHandleAttr(S, D, AL);
    break;

  case ParsedAttr::AT_ReleaseHandle:
    handleHandleAttr<ReleaseHandleAttr>(S, D, AL);
    break;

  case ParsedAttr::AT_UnsafeBufferUsage:
    handleUnsafeBufferUsage<UnsafeBufferUsageAttr>(S, D, AL);
    break;

  case ParsedAttr::AT_UseHandle:
    handleHandleAttr<UseHandleAttr>(S, D, AL);
    break;

  case ParsedAttr::AT_EnforceTCB:
    handleEnforceTCBAttr<EnforceTCBAttr, EnforceTCBLeafAttr>(S, D, AL);
    break;

  case ParsedAttr::AT_EnforceTCBLeaf:
    handleEnforceTCBAttr<EnforceTCBLeafAttr, EnforceTCBAttr>(S, D, AL);
    break;

  case ParsedAttr::AT_BuiltinAlias:
    handleBuiltinAliasAttr(S, D, AL);
    break;

  case ParsedAttr::AT_PreferredType:
    handlePreferredTypeAttr(S, D, AL);
    break;

  case ParsedAttr::AT_UsingIfExists:
    handleSimpleAttribute<UsingIfExistsAttr>(S, D, AL);
    break;

  case ParsedAttr::AT_TypeNullable:
    handleNullableTypeAttr(S, D, AL);
    break;

  case ParsedAttr::AT_VTablePointerAuthentication:
    handleVTablePointerAuthentication(S, D, AL);
    break;
  }
}

static bool isKernelDecl(Decl *D) {
  const FunctionType *FnTy = D->getFunctionType();
  return D->hasAttr<DeviceKernelAttr>() ||
         (FnTy && FnTy->getCallConv() == CallingConv::CC_DeviceKernel) ||
         D->hasAttr<CUDAGlobalAttr>();
}

void Sema::ProcessDeclAttributeList(
    Scope *S, Decl *D, const ParsedAttributesView &AttrList,
    const ProcessDeclAttributeOptions &Options) {
  if (AttrList.empty())
    return;

  for (const ParsedAttr &AL : AttrList)
    ProcessDeclAttribute(*this, S, D, AL, Options);

  // FIXME: We should be able to handle these cases in TableGen.
  // GCC accepts
  // static int a9 __attribute__((weakref));
  // but that looks really pointless. We reject it.
  if (D->hasAttr<WeakRefAttr>() && !D->hasAttr<AliasAttr>()) {
    Diag(AttrList.begin()->getLoc(), diag::err_attribute_weakref_without_alias)
        << cast<NamedDecl>(D);
    D->dropAttr<WeakRefAttr>();
    return;
  }

  // FIXME: We should be able to handle this in TableGen as well. It would be
  // good to have a way to specify "these attributes must appear as a group",
  // for these. Additionally, it would be good to have a way to specify "these
  // attribute must never appear as a group" for attributes like cold and hot.
  if (!(D->hasAttr<DeviceKernelAttr>() ||
        (D->hasAttr<CUDAGlobalAttr>() &&
         Context.getTargetInfo().getTriple().isSPIRV()))) {
    // These attributes cannot be applied to a non-kernel function.
    if (const auto *A = D->getAttr<ReqdWorkGroupSizeAttr>()) {
      // FIXME: This emits a different error message than
      // diag::err_attribute_wrong_decl_type + ExpectedKernelFunction.
      Diag(D->getLocation(), diag::err_opencl_kernel_attr) << A;
      D->setInvalidDecl();
    } else if (const auto *A = D->getAttr<WorkGroupSizeHintAttr>()) {
      Diag(D->getLocation(), diag::err_opencl_kernel_attr) << A;
      D->setInvalidDecl();
    } else if (const auto *A = D->getAttr<VecTypeHintAttr>()) {
      Diag(D->getLocation(), diag::err_opencl_kernel_attr) << A;
      D->setInvalidDecl();
    } else if (const auto *A = D->getAttr<OpenCLIntelReqdSubGroupSizeAttr>()) {
      Diag(D->getLocation(), diag::err_opencl_kernel_attr) << A;
      D->setInvalidDecl();
    }
  }
  if (!isKernelDecl(D)) {
    if (const auto *A = D->getAttr<AMDGPUFlatWorkGroupSizeAttr>()) {
      Diag(D->getLocation(), diag::err_attribute_wrong_decl_type)
          << A << A->isRegularKeywordAttribute() << ExpectedKernelFunction;
      D->setInvalidDecl();
    } else if (const auto *A = D->getAttr<AMDGPUWavesPerEUAttr>()) {
      Diag(D->getLocation(), diag::err_attribute_wrong_decl_type)
          << A << A->isRegularKeywordAttribute() << ExpectedKernelFunction;
      D->setInvalidDecl();
    } else if (const auto *A = D->getAttr<AMDGPUNumSGPRAttr>()) {
      Diag(D->getLocation(), diag::err_attribute_wrong_decl_type)
          << A << A->isRegularKeywordAttribute() << ExpectedKernelFunction;
      D->setInvalidDecl();
    } else if (const auto *A = D->getAttr<AMDGPUNumVGPRAttr>()) {
      Diag(D->getLocation(), diag::err_attribute_wrong_decl_type)
          << A << A->isRegularKeywordAttribute() << ExpectedKernelFunction;
      D->setInvalidDecl();
    }
  }

  // Do not permit 'constructor' or 'destructor' attributes on __device__ code.
  if (getLangOpts().CUDAIsDevice && D->hasAttr<CUDADeviceAttr>() &&
      (D->hasAttr<ConstructorAttr>() || D->hasAttr<DestructorAttr>()) &&
      !getLangOpts().GPUAllowDeviceInit) {
    Diag(D->getLocation(), diag::err_cuda_ctor_dtor_attrs)
        << (D->hasAttr<ConstructorAttr>() ? "constructors" : "destructors");
    D->setInvalidDecl();
  }

  // Do this check after processing D's attributes because the attribute
  // objc_method_family can change whether the given method is in the init
  // family, and it can be applied after objc_designated_initializer. This is a
  // bit of a hack, but we need it to be compatible with versions of clang that
  // processed the attribute list in the wrong order.
  if (D->hasAttr<ObjCDesignatedInitializerAttr>() &&
      cast<ObjCMethodDecl>(D)->getMethodFamily() != OMF_init) {
    Diag(D->getLocation(), diag::err_designated_init_attr_non_init);
    D->dropAttr<ObjCDesignatedInitializerAttr>();
  }
}

void Sema::ProcessDeclAttributeDelayed(Decl *D,
                                       const ParsedAttributesView &AttrList) {
  for (const ParsedAttr &AL : AttrList)
    if (AL.getKind() == ParsedAttr::AT_TransparentUnion) {
      handleTransparentUnionAttr(*this, D, AL);
      break;
    }

  // For BPFPreserveAccessIndexAttr, we want to populate the attributes
  // to fields and inner records as well.
  if (D && D->hasAttr<BPFPreserveAccessIndexAttr>())
    BPF().handlePreserveAIRecord(cast<RecordDecl>(D));
}

bool Sema::ProcessAccessDeclAttributeList(
    AccessSpecDecl *ASDecl, const ParsedAttributesView &AttrList) {
  for (const ParsedAttr &AL : AttrList) {
    if (AL.getKind() == ParsedAttr::AT_Annotate) {
      ProcessDeclAttribute(*this, nullptr, ASDecl, AL,
                           ProcessDeclAttributeOptions());
    } else {
      Diag(AL.getLoc(), diag::err_only_annotate_after_access_spec);
      return true;
    }
  }
  return false;
}

/// checkUnusedDeclAttributes - Check a list of attributes to see if it
/// contains any decl attributes that we should warn about.
static void checkUnusedDeclAttributes(Sema &S, const ParsedAttributesView &A) {
  for (const ParsedAttr &AL : A) {
    // Only warn if the attribute is an unignored, non-type attribute.
    if (AL.isUsedAsTypeAttr() || AL.isInvalid())
      continue;
    if (AL.getKind() == ParsedAttr::IgnoredAttribute)
      continue;

    if (AL.getKind() == ParsedAttr::UnknownAttribute) {
      S.DiagnoseUnknownAttribute(AL);
    } else {
      S.Diag(AL.getLoc(), diag::warn_attribute_not_on_decl) << AL
                                                            << AL.getRange();
    }
  }
}

void Sema::checkUnusedDeclAttributes(Declarator &D) {
  ::checkUnusedDeclAttributes(*this, D.getDeclarationAttributes());
  ::checkUnusedDeclAttributes(*this, D.getDeclSpec().getAttributes());
  ::checkUnusedDeclAttributes(*this, D.getAttributes());
  for (unsigned i = 0, e = D.getNumTypeObjects(); i != e; ++i)
    ::checkUnusedDeclAttributes(*this, D.getTypeObject(i).getAttrs());
}

void Sema::DiagnoseUnknownAttribute(const ParsedAttr &AL) {
  SourceRange NR = AL.getNormalizedRange();
  StringRef ScopeName = AL.getNormalizedScopeName();
  std::optional<StringRef> CorrectedScopeName =
      AL.tryGetCorrectedScopeName(ScopeName);
  if (CorrectedScopeName) {
    ScopeName = *CorrectedScopeName;
  }

  StringRef AttrName = AL.getNormalizedAttrName(ScopeName);
  std::optional<StringRef> CorrectedAttrName = AL.tryGetCorrectedAttrName(
      ScopeName, AttrName, Context.getTargetInfo(), getLangOpts());
  if (CorrectedAttrName) {
    AttrName = *CorrectedAttrName;
  }

  if (CorrectedScopeName || CorrectedAttrName) {
    std::string CorrectedFullName =
        AL.getNormalizedFullName(ScopeName, AttrName);
    SemaDiagnosticBuilder D =
        Diag(CorrectedScopeName ? NR.getBegin() : AL.getRange().getBegin(),
             diag::warn_unknown_attribute_ignored_suggestion);

    D << AL << CorrectedFullName;

    if (AL.isExplicitScope()) {
      D << FixItHint::CreateReplacement(NR, CorrectedFullName) << NR;
    } else {
      if (CorrectedScopeName) {
        D << FixItHint::CreateReplacement(SourceRange(AL.getScopeLoc()),
                                          ScopeName);
      }
      if (CorrectedAttrName) {
        D << FixItHint::CreateReplacement(AL.getRange(), AttrName);
      }
    }
  } else {
    Diag(NR.getBegin(), diag::warn_unknown_attribute_ignored) << AL << NR;
  }
}

NamedDecl *Sema::DeclClonePragmaWeak(NamedDecl *ND, const IdentifierInfo *II,
                                     SourceLocation Loc) {
  assert(isa<FunctionDecl>(ND) || isa<VarDecl>(ND));
  NamedDecl *NewD = nullptr;
  if (auto *FD = dyn_cast<FunctionDecl>(ND)) {
    FunctionDecl *NewFD;
    // FIXME: Missing call to CheckFunctionDeclaration().
    // FIXME: Mangling?
    // FIXME: Is the qualifier info correct?
    // FIXME: Is the DeclContext correct?
    NewFD = FunctionDecl::Create(
        FD->getASTContext(), FD->getDeclContext(), Loc, Loc,
        DeclarationName(II), FD->getType(), FD->getTypeSourceInfo(), SC_None,
        getCurFPFeatures().isFPConstrained(), false /*isInlineSpecified*/,
        FD->hasPrototype(), ConstexprSpecKind::Unspecified,
        FD->getTrailingRequiresClause());
    NewD = NewFD;

    if (FD->getQualifier())
      NewFD->setQualifierInfo(FD->getQualifierLoc());

    // Fake up parameter variables; they are declared as if this were
    // a typedef.
    QualType FDTy = FD->getType();
    if (const auto *FT = FDTy->getAs<FunctionProtoType>()) {
      SmallVector<ParmVarDecl*, 16> Params;
      for (const auto &AI : FT->param_types()) {
        ParmVarDecl *Param = BuildParmVarDeclForTypedef(NewFD, Loc, AI);
        Param->setScopeInfo(0, Params.size());
        Params.push_back(Param);
      }
      NewFD->setParams(Params);
    }
  } else if (auto *VD = dyn_cast<VarDecl>(ND)) {
    NewD = VarDecl::Create(VD->getASTContext(), VD->getDeclContext(),
                           VD->getInnerLocStart(), VD->getLocation(), II,
                           VD->getType(), VD->getTypeSourceInfo(),
                           VD->getStorageClass());
    if (VD->getQualifier())
      cast<VarDecl>(NewD)->setQualifierInfo(VD->getQualifierLoc());
  }
  return NewD;
}

void Sema::DeclApplyPragmaWeak(Scope *S, NamedDecl *ND, const WeakInfo &W) {
  if (W.getAlias()) { // clone decl, impersonate __attribute(weak,alias(...))
    IdentifierInfo *NDId = ND->getIdentifier();
    NamedDecl *NewD = DeclClonePragmaWeak(ND, W.getAlias(), W.getLocation());
    NewD->addAttr(
        AliasAttr::CreateImplicit(Context, NDId->getName(), W.getLocation()));
    NewD->addAttr(WeakAttr::CreateImplicit(Context, W.getLocation()));
    WeakTopLevelDecl.push_back(NewD);
    // FIXME: "hideous" code from Sema::LazilyCreateBuiltin
    // to insert Decl at TU scope, sorry.
    DeclContext *SavedContext = CurContext;
    CurContext = Context.getTranslationUnitDecl();
    NewD->setDeclContext(CurContext);
    NewD->setLexicalDeclContext(CurContext);
    PushOnScopeChains(NewD, S);
    CurContext = SavedContext;
  } else { // just add weak to existing
    ND->addAttr(WeakAttr::CreateImplicit(Context, W.getLocation()));
  }
}

void Sema::ProcessPragmaWeak(Scope *S, Decl *D) {
  // It's valid to "forward-declare" #pragma weak, in which case we
  // have to do this.
  LoadExternalWeakUndeclaredIdentifiers();
  if (WeakUndeclaredIdentifiers.empty())
    return;
  NamedDecl *ND = nullptr;
  if (auto *VD = dyn_cast<VarDecl>(D))
    if (VD->isExternC())
      ND = VD;
  if (auto *FD = dyn_cast<FunctionDecl>(D))
    if (FD->isExternC())
      ND = FD;
  if (!ND)
    return;
  if (IdentifierInfo *Id = ND->getIdentifier()) {
    auto I = WeakUndeclaredIdentifiers.find(Id);
    if (I != WeakUndeclaredIdentifiers.end()) {
      auto &WeakInfos = I->second;
      for (const auto &W : WeakInfos)
        DeclApplyPragmaWeak(S, ND, W);
      std::remove_reference_t<decltype(WeakInfos)> EmptyWeakInfos;
      WeakInfos.swap(EmptyWeakInfos);
    }
  }
}

/// ProcessDeclAttributes - Given a declarator (PD) with attributes indicated in
/// it, apply them to D.  This is a bit tricky because PD can have attributes
/// specified in many different places, and we need to find and apply them all.
void Sema::ProcessDeclAttributes(Scope *S, Decl *D, const Declarator &PD) {
  // Ordering of attributes can be important, so we take care to process
  // attributes in the order in which they appeared in the source code.

  auto ProcessAttributesWithSliding =
      [&](const ParsedAttributesView &Src,
          const ProcessDeclAttributeOptions &Options) {
        ParsedAttributesView NonSlidingAttrs;
        for (ParsedAttr &AL : Src) {
          // FIXME: this sliding is specific to standard attributes and should
          // eventually be deprecated and removed as those are not intended to
          // slide to anything.
          if ((AL.isStandardAttributeSyntax() || AL.isAlignas()) &&
              AL.slidesFromDeclToDeclSpecLegacyBehavior()) {
            // Skip processing the attribute, but do check if it appertains to
            // the declaration. This is needed for the `MatrixType` attribute,
            // which, despite being a type attribute, defines a `SubjectList`
            // that only allows it to be used on typedef declarations.
            AL.diagnoseAppertainsTo(*this, D);
          } else {
            NonSlidingAttrs.addAtEnd(&AL);
          }
        }
        ProcessDeclAttributeList(S, D, NonSlidingAttrs, Options);
      };

  // First, process attributes that appeared on the declaration itself (but
  // only if they don't have the legacy behavior of "sliding" to the DeclSepc).
  ProcessAttributesWithSliding(PD.getDeclarationAttributes(), {});

  // Apply decl attributes from the DeclSpec if present.
  ProcessAttributesWithSliding(PD.getDeclSpec().getAttributes(),
                               ProcessDeclAttributeOptions()
                                   .WithIncludeCXX11Attributes(false)
                                   .WithIgnoreTypeAttributes(true));

  // Walk the declarator structure, applying decl attributes that were in a type
  // position to the decl itself.  This handles cases like:
  //   int *__attr__(x)** D;
  // when X is a decl attribute.
  for (unsigned i = 0, e = PD.getNumTypeObjects(); i != e; ++i) {
    ProcessDeclAttributeList(S, D, PD.getTypeObject(i).getAttrs(),
                             ProcessDeclAttributeOptions()
                                 .WithIncludeCXX11Attributes(false)
                                 .WithIgnoreTypeAttributes(true));
  }

  // Finally, apply any attributes on the decl itself.
  ProcessDeclAttributeList(S, D, PD.getAttributes());

  // Apply additional attributes specified by '#pragma clang attribute'.
  AddPragmaAttributes(S, D);

  // Look for API notes that map to attributes.
  ProcessAPINotes(D);
}

/// Is the given declaration allowed to use a forbidden type?
/// If so, it'll still be annotated with an attribute that makes it
/// illegal to actually use.
static bool isForbiddenTypeAllowed(Sema &S, Decl *D,
                                   const DelayedDiagnostic &diag,
                                   UnavailableAttr::ImplicitReason &reason) {
  // Private ivars are always okay.  Unfortunately, people don't
  // always properly make their ivars private, even in system headers.
  // Plus we need to make fields okay, too.
  if (!isa<FieldDecl>(D) && !isa<ObjCPropertyDecl>(D) &&
      !isa<FunctionDecl>(D))
    return false;

  // Silently accept unsupported uses of __weak in both user and system
  // declarations when it's been disabled, for ease of integration with
  // -fno-objc-arc files.  We do have to take some care against attempts
  // to define such things;  for now, we've only done that for ivars
  // and properties.
  if ((isa<ObjCIvarDecl>(D) || isa<ObjCPropertyDecl>(D))) {
    if (diag.getForbiddenTypeDiagnostic() == diag::err_arc_weak_disabled ||
        diag.getForbiddenTypeDiagnostic() == diag::err_arc_weak_no_runtime) {
      reason = UnavailableAttr::IR_ForbiddenWeak;
      return true;
    }
  }

  // Allow all sorts of things in system headers.
  if (S.Context.getSourceManager().isInSystemHeader(D->getLocation())) {
    // Currently, all the failures dealt with this way are due to ARC
    // restrictions.
    reason = UnavailableAttr::IR_ARCForbiddenType;
    return true;
  }

  return false;
}

/// Handle a delayed forbidden-type diagnostic.
static void handleDelayedForbiddenType(Sema &S, DelayedDiagnostic &DD,
                                       Decl *D) {
  auto Reason = UnavailableAttr::IR_None;
  if (D && isForbiddenTypeAllowed(S, D, DD, Reason)) {
    assert(Reason && "didn't set reason?");
    D->addAttr(UnavailableAttr::CreateImplicit(S.Context, "", Reason, DD.Loc));
    return;
  }
  if (S.getLangOpts().ObjCAutoRefCount)
    if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
      // FIXME: we may want to suppress diagnostics for all
      // kind of forbidden type messages on unavailable functions.
      if (FD->hasAttr<UnavailableAttr>() &&
          DD.getForbiddenTypeDiagnostic() ==
              diag::err_arc_array_param_no_ownership) {
        DD.Triggered = true;
        return;
      }
    }

  S.Diag(DD.Loc, DD.getForbiddenTypeDiagnostic())
      << DD.getForbiddenTypeOperand() << DD.getForbiddenTypeArgument();
  DD.Triggered = true;
}


void Sema::PopParsingDeclaration(ParsingDeclState state, Decl *decl) {
  assert(DelayedDiagnostics.getCurrentPool());
  DelayedDiagnosticPool &poppedPool = *DelayedDiagnostics.getCurrentPool();
  DelayedDiagnostics.popWithoutEmitting(state);

  // When delaying diagnostics to run in the context of a parsed
  // declaration, we only want to actually emit anything if parsing
  // succeeds.
  if (!decl) return;

  // We emit all the active diagnostics in this pool or any of its
  // parents.  In general, we'll get one pool for the decl spec
  // and a child pool for each declarator; in a decl group like:
  //   deprecated_typedef foo, *bar, baz();
  // only the declarator pops will be passed decls.  This is correct;
  // we really do need to consider delayed diagnostics from the decl spec
  // for each of the different declarations.
  const DelayedDiagnosticPool *pool = &poppedPool;
  do {
    bool AnyAccessFailures = false;
    for (DelayedDiagnosticPool::pool_iterator
           i = pool->pool_begin(), e = pool->pool_end(); i != e; ++i) {
      // This const_cast is a bit lame.  Really, Triggered should be mutable.
      DelayedDiagnostic &diag = const_cast<DelayedDiagnostic&>(*i);
      if (diag.Triggered)
        continue;

      switch (diag.Kind) {
      case DelayedDiagnostic::Availability:
        // Don't bother giving deprecation/unavailable diagnostics if
        // the decl is invalid.
        if (!decl->isInvalidDecl())
          handleDelayedAvailabilityCheck(diag, decl);
        break;

      case DelayedDiagnostic::Access:
        // Only produce one access control diagnostic for a structured binding
        // declaration: we don't need to tell the user that all the fields are
        // inaccessible one at a time.
        if (AnyAccessFailures && isa<DecompositionDecl>(decl))
          continue;
        HandleDelayedAccessCheck(diag, decl);
        if (diag.Triggered)
          AnyAccessFailures = true;
        break;

      case DelayedDiagnostic::ForbiddenType:
        handleDelayedForbiddenType(*this, diag, decl);
        break;
      }
    }
  } while ((pool = pool->getParent()));
}

void Sema::redelayDiagnostics(DelayedDiagnosticPool &pool) {
  DelayedDiagnosticPool *curPool = DelayedDiagnostics.getCurrentPool();
  assert(curPool && "re-emitting in undelayed context not supported");
  curPool->steal(pool);
}
