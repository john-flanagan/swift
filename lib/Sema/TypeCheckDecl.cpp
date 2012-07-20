//===--- TypeCheckDecl.cpp - Type Checking for Declarations ---------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements semantic analysis for declarations.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "ArchetypeBuilder.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Attr.h"
#include "llvm/ADT/Twine.h"
using namespace swift;

namespace {
class DeclChecker : public DeclVisitor<DeclChecker> {
  
public:
  TypeChecker &TC;

  // For library-style parsing, we need to make two passes over the global
  // scope.  These booleans indicate whether this is currently the first or
  // second pass over the global scope (or neither, if we're in a context where
  // we only visit each decl once).
  bool IsFirstPass;
  bool IsSecondPass;

  DeclChecker(TypeChecker &TC, bool IsFirstPass, bool IsSecondPass)
      : TC(TC), IsFirstPass(IsFirstPass), IsSecondPass(IsSecondPass) {}

  //===--------------------------------------------------------------------===//
  // Helper Functions.
  //===--------------------------------------------------------------------===//

  void validateAttributes(ValueDecl *VD);

  /// \brief Check the list of inherited protocols on the declaration D.
  void checkInherited(Decl *D, MutableArrayRef<TypeLoc> Inherited) {
    // Check the list of inherited protocols.
    for (unsigned i = 0, e = Inherited.size(); i != e; ++i) {
      if (TC.validateType(Inherited[i], IsFirstPass)) {
        Inherited[i].setInvalidType(TC.Context);
        continue;
      }

      if (!Inherited[i].getType()->isExistentialType() &&
          !Inherited[i].getType()->is<ErrorType>()) {
        // FIXME: Terrible location information.
        TC.diagnose(D->getStartLoc(), diag::nonprotocol_inherit,
                    Inherited[i].getType());
      }
    }
  }

  void checkExplicitConformance(Decl *D, Type T,
                                MutableArrayRef<TypeLoc> Inherited) {
    for (auto InheritedTy : Inherited) {
      // FIXME: Poor location info.
      SmallVector<ProtocolDecl *, 4> InheritedProtos;
      if (InheritedTy.getType()->isExistentialType(InheritedProtos))
        for (auto Proto : InheritedProtos)
          TC.conformsToProtocol(T, Proto, nullptr, D->getStartLoc());
    }
  }

  void checkGenericParams(GenericParamList *GenericParams) {
    if (!GenericParams)
      return;

    // Assign archetypes to each of the generic parameters.
    ArchetypeBuilder Builder(TC);
    unsigned Index = 0;
    for (auto GP : *GenericParams) {
      auto TypeParam = GP.getAsTypeParam();

      // Check the constraints on the type parameter.
      checkInherited(TypeParam, TypeParam->getInherited());

      // Add the generic parameter to the builder.
      Builder.addGenericParameter(TypeParam, Index++);
    }

    // Add the requirements clause to the builder, validating only those
    // types that need to be complete at this point.
    // FIXME: Tell the type validator not to assert about unresolved types.
    for (auto &Req : GenericParams->getRequirements()) {
      switch (Req.getKind()) {
      case RequirementKind::Conformance: {
        // FIXME: TypeLoc info?
        TypeLoc TempLoc{ Req.getProtocol() };
        if (TC.validateType(TempLoc, IsFirstPass)) {
          Req.overrideProtocol(ErrorType::get(TC.Context));
          continue;
        }

        if (!Req.getProtocol()->isExistentialType()) {
          TC.diagnose(GenericParams->getRequiresLoc(),
                      diag::requires_conformance_nonprotocol,
                      Req.getSubject(), Req.getProtocol());
          Req.overrideProtocol(ErrorType::get(TC.Context));
          continue;
        }
        break;
      }

      case RequirementKind::SameType:
        break;
      }

      Builder.addRequirement(Req);
    }

    // Wire up the archetypes.
    llvm::DenseMap<TypeAliasDecl *, ArchetypeType *> Archetypes
      = Builder.assignArchetypes();
    for (auto Arch : Archetypes) {
      Arch.first->getUnderlyingTypeLoc() = TypeLoc(Arch.second);
    }

    // Validate the types in the requirements clause.
    for (auto &Req : GenericParams->getRequirements()) {
      switch (Req.getKind()) {
        case RequirementKind::Conformance: {
          // FIXME: TypeLoc info?
          TypeLoc TempLoc{ Req.getSubject() };
          if (TC.validateType(TempLoc, IsFirstPass)) {
            Req.overrideSubject(ErrorType::get(TC.Context));
            continue;
          }
          break;
        }

        case RequirementKind::SameType:
          // FIXME: TypeLoc info?
          TypeLoc TempLoc{ Req.getFirstType() };
          if (TC.validateType(TempLoc, IsFirstPass)) {
            Req.overrideFirstType(ErrorType::get(TC.Context));
            continue;
          }

          // FIXME: TypeLoc info?
          TempLoc = TypeLoc{ Req.getSecondType() };
          if (TC.validateType(TempLoc, IsFirstPass)) {
            Req.overrideSecondType(ErrorType::get(TC.Context));
            continue;
          }
          break;
      }
      
      Builder.addRequirement(Req);
    }
  }

  //===--------------------------------------------------------------------===//
  // Visit Methods.
  //===--------------------------------------------------------------------===//

  void visitImportDecl(ImportDecl *ID) {
    // Nothing to do.
  }

  void visitBoundVars(Pattern *P) {
    switch (P->getKind()) {
    // Recurse into patterns.
    case PatternKind::Tuple:
      for (auto &field : cast<TuplePattern>(P)->getFields())
        visitBoundVars(field.getPattern());
      return;
    case PatternKind::Paren:
      return visitBoundVars(cast<ParenPattern>(P)->getSubPattern());
    case PatternKind::Typed:
      return visitBoundVars(cast<TypedPattern>(P)->getSubPattern());

    // Handle vars.
    case PatternKind::Named: {
      VarDecl *VD = cast<NamedPattern>(P)->getDecl();

      if (!VD->getType()->isMaterializable()) {
        TC.diagnose(VD->getStartLoc(), diag::var_type_not_materializable,
                    VD->getType());
        VD->overwriteType(ErrorType::get(TC.Context));
      }

      validateAttributes(VD);
      return;
    }

    // Handle non-vars.
    case PatternKind::Any:
      return;
    }
    llvm_unreachable("bad pattern kind!");
  }

  void visitPatternBindingDecl(PatternBindingDecl *PBD) {
    bool DelayCheckingPattern = TC.TU.Kind != TranslationUnit::Library &&
                                PBD->getDeclContext()->isModuleContext();
    if (IsSecondPass && !DelayCheckingPattern) {
      if (PBD->getInit() && PBD->getPattern()->hasType()) {
        Expr *Init = PBD->getInit();
        Type DestTy = PBD->getPattern()->getType();
        if (TC.typeCheckExpression(Init, DestTy)) {
          if (DestTy)
            TC.diagnose(PBD->getStartLoc(), diag::while_converting_var_init,
                        DestTy);
        } else {
          PBD->setInit(Init);
        }
      }
      return;
    }
    if (PBD->getInit() && !IsFirstPass) {
      Type DestTy;
      if (isa<TypedPattern>(PBD->getPattern())) {
        if (TC.typeCheckPattern(PBD->getPattern(), /*isFirstPass*/false))
          return;
        DestTy = PBD->getPattern()->getType();
      }
      Expr *Init = PBD->getInit();
      if (TC.typeCheckExpression(Init, DestTy)) {
        if (DestTy)
          TC.diagnose(PBD->getStartLoc(), diag::while_converting_var_init,
                      DestTy);
        return;
      }
      if (!DestTy) {
        Expr *newInit = TC.convertToMaterializable(Init);
        if (newInit) Init = newInit;
      }
      PBD->setInit(Init);
      if (!DestTy) {
        if (TC.coerceToType(PBD->getPattern(), Init->getType(),
                            /*isFirstPass*/false))
          return;
      }
    } else if (!IsFirstPass || !DelayCheckingPattern) {
      if (TC.typeCheckPattern(PBD->getPattern(), IsFirstPass))
        return;
    }
    visitBoundVars(PBD->getPattern());
  }

  void visitSubscriptDecl(SubscriptDecl *SD) {
    if (IsSecondPass)
      return;

    // The getter and setter functions will be type-checked separately.
    if (!SD->getDeclContext()->isTypeContext())
      TC.diagnose(SD->getStartLoc(), diag::subscript_not_member);

    TC.validateType(SD->getElementTypeLoc(), IsFirstPass);

    if (!TC.typeCheckPattern(SD->getIndices(), IsFirstPass))  {
      SD->setType(FunctionType::get(SD->getIndices()->getType(),
                                    SD->getElementType(), TC.Context));
    }
  }
  
  void visitTypeAliasDecl(TypeAliasDecl *TAD) {
    if (!IsSecondPass) {
      TC.validateType(TAD->getUnderlyingTypeLoc(), IsFirstPass);
      if (!isa<ProtocolDecl>(TAD->getDeclContext()))
        checkInherited(TAD, TAD->getInherited());
    }

    if (!IsFirstPass)
      checkExplicitConformance(TAD, TAD->getDeclaredType(),
                               TAD->getInherited());
  }

  void visitOneOfDecl(OneOfDecl *OOD) {
    if (!IsSecondPass) {
      checkInherited(OOD, OOD->getInherited());
      checkGenericParams(OOD->getGenericParams());
    }
    
    for (Decl *member : OOD->getMembers())
      visit(member);
    
    if (!IsFirstPass)
      checkExplicitConformance(OOD, OOD->getDeclaredType(),
                               OOD->getInherited());
  }

  void visitStructDecl(StructDecl *SD) {
    if (!IsSecondPass) {
      checkInherited(SD, SD->getInherited());
      checkGenericParams(SD->getGenericParams());
    }

    for (Decl *Member : SD->getMembers()) {
      visit(Member);
    }

    if (!IsSecondPass) {
      // FIXME: We should come up with a better way to represent this implied
      // constructor.
      SmallVector<TupleTypeElt, 8> TupleElts;
      for (Decl *Member : SD->getMembers())
        if (VarDecl *VarD = dyn_cast<VarDecl>(Member))
          if (!VarD->isProperty())
            TupleElts.push_back(TupleTypeElt(VarD->getType(),
                                             VarD->getName()));
      TupleType *TT = TupleType::get(TupleElts, TC.Context);
      Type CreateTy = FunctionType::get(TT, SD->getDeclaredTypeInContext(),
                                        TC.Context);
      auto ElementCtor = cast<OneOfElementDecl>(SD->getMembers().back());
      ElementCtor->setType(CreateTy);
      ElementCtor->getArgumentTypeLoc() = TypeLoc(TT);
    }

    if (!IsFirstPass)
      checkExplicitConformance(SD, SD->getDeclaredType(),
                               SD->getInherited());
  }

  void visitClassDecl(ClassDecl *CD) {
    if (!IsSecondPass) {
      checkInherited(CD, CD->getInherited());
      checkGenericParams(CD->getGenericParams());
    }

    for (Decl *Member : CD->getMembers())
      visit(Member);
    
    if (!IsFirstPass)
      checkExplicitConformance(CD, CD->getDeclaredType(),
                               CD->getInherited());
  }

  void visitProtocolDecl(ProtocolDecl *PD) {
    if (IsSecondPass)
      return;

    checkInherited(PD, PD->getInherited());
    
    // Assign archetypes each of the associated types.
    // FIXME: We need to build equivalence classes of associated types first,
    // then assign an archtype to each equivalence class.
    // FIXME: As part of building the equivalence class, find all of the
    // protocols that each archetype should conform to.
    for (auto Member : PD->getMembers()) {
      if (auto AssocType = dyn_cast<TypeAliasDecl>(Member)) {
        checkInherited(AssocType, AssocType->getInherited());

        Optional<unsigned> Index;
        // FIXME: Find a better way to identify the 'This' archetype.
        if (AssocType->getName().str().equals("This"))
          Index = 0;
        SmallVector<Type, 4> InheritedTypes;
        for (TypeLoc T : AssocType->getInherited())
          InheritedTypes.push_back(T.getType());
        Type UnderlyingTy =
            ArchetypeType::getNew(TC.Context, AssocType->getName().str(),
                                  InheritedTypes, Index);
        AssocType->getUnderlyingTypeLoc() = TypeLoc(UnderlyingTy);
      }
    }

    // Check the members.
    for (auto Member : PD->getMembers())
      visit(Member);
  }
  
  void visitVarDecl(VarDecl *VD) {
    // Delay type-checking on VarDecls until we see the corresponding
    // PatternBindingDecl.
  }

  void visitFuncDecl(FuncDecl *FD) {
    if (IsSecondPass)
      return;

    FuncExpr *body = FD->getBody();

    // Before anything else, set up the 'this' argument correctly.
    bool isInstanceFunc = false;
    if (Type thisType = FD->computeThisType()) {
      TypedPattern *thisPattern =
        cast<TypedPattern>(body->getParamPatterns()[0]);
      if (thisPattern->hasType()) {
        assert(thisPattern->getType().getPointer() == thisType.getPointer());
      } else {
        thisPattern->setType(thisType);
      }
      isInstanceFunc = true;
    }

    checkGenericParams(FD->getGenericParams());

    TC.semaFuncExpr(body, IsFirstPass);
    FD->setType(body->getType());

    validateAttributes(FD);
  }

  void visitOneOfElementDecl(OneOfElementDecl *ED) {
    if (IsSecondPass)
      return;

    // Ignore OneOfElementDecls in structs.
    // FIXME: Remove once the struct hack is fixed.
    OneOfDecl *OOD = dyn_cast<OneOfDecl>(ED->getDeclContext());
    if (!OOD)
      return;

    Type ElemTy = OOD->getDeclaredTypeInContext();

    // If we have a simple element, just set the type.
    if (ED->getArgumentType().isNull()) {
      ED->setType(ElemTy);
      return;
    }

    // We have an element with an argument type; validate the argument,
    // then compute a function type.
    if (TC.validateType(ED->getArgumentTypeLoc(), IsFirstPass))
      return;

    ED->setType(FunctionType::get(ED->getArgumentType(), ElemTy, TC.Context));

    // Require the carried type to be materializable.
    if (!ED->getArgumentType()->isMaterializable()) {
      TC.diagnose(ED->getLoc(),
                  diag::oneof_element_not_materializable);
    }
  }

  void visitExtensionDecl(ExtensionDecl *ED) {
    if (!IsSecondPass) {
      TC.validateType(ED->getExtendedTypeLoc(), IsFirstPass);

      Type ExtendedTy = ED->getExtendedType();
      if (!ExtendedTy->is<OneOfType>() && !ExtendedTy->is<StructType>() &&
          !ExtendedTy->is<ClassType>() && !ExtendedTy->is<ErrorType>() &&
          !ExtendedTy->is<UnboundGenericType>()) {
        TC.diagnose(ED->getStartLoc(), diag::non_nominal_extension,
                    ExtendedTy->is<ProtocolType>(), ExtendedTy);
        // FIXME: It would be nice to point out where we found the named type
        // declaration, if any.
      }
    
      checkInherited(ED, ED->getInherited());
    }

    for (Decl *Member : ED->getMembers())
      visit(Member);

    if (!IsFirstPass)
      checkExplicitConformance(ED, ED->getExtendedType(),
                               ED->getInherited());
  }

  void visitTopLevelCodeDecl(TopLevelCodeDecl *TLCD) {
    // See swift::performTypeChecking for TopLevelCodeDecl handling.
    llvm_unreachable("TopLevelCodeDecls are handled elsewhere");
  }

  void visitConstructorDecl(ConstructorDecl *CD) {
    if (IsSecondPass)
      return;

    if (!CD->getDeclContext()->isTypeContext())
      TC.diagnose(CD->getStartLoc(), diag::constructor_not_member);

    checkGenericParams(CD->getGenericParams());

    Type ThisTy = CD->computeThisType();
    CD->getImplicitThisDecl()->setType(ThisTy);

    if (TC.typeCheckPattern(CD->getArguments(), IsFirstPass)) {
      CD->setType(ErrorType::get(TC.Context));
    } else {
      Type FnTy;
      if (CD->getGenericParams())
        FnTy = PolymorphicFunctionType::get(CD->getArguments()->getType(),
                                            ThisTy, CD->getGenericParams(),
                                            TC.Context);
      else
        FnTy = FunctionType::get(CD->getArguments()->getType(),
                                 ThisTy, TC.Context);
      CD->setType(FnTy);
    }

    validateAttributes(CD);
  }

  void visitDestructorDecl(DestructorDecl *DD) {
    if (IsSecondPass)
      return;

    if (!isa<ClassDecl>(DD->getDeclContext()))
      TC.diagnose(DD->getStartLoc(), diag::destructor_not_member);

    Type ThisTy = DD->computeThisType();
    Type FnTy = FunctionType::get(DD->computeThisType(),
                                 TupleType::getEmpty(TC.Context),
                                 TC.Context);
    DD->setType(FnTy);
    DD->getImplicitThisDecl()->setType(ThisTy);

    validateAttributes(DD);
  }
};
}; // end anonymous namespace.


void TypeChecker::typeCheckDecl(Decl *D, bool isFirstPass) {
  bool isSecondPass = !isFirstPass && D->getDeclContext()->isModuleContext();
  DeclChecker(*this, isFirstPass, isSecondPass).visit(D);
}

/// validateAttributes - Check that the func/var declaration attributes are ok.
void DeclChecker::validateAttributes(ValueDecl *VD) {
  const DeclAttributes &Attrs = VD->getAttrs();
  Type Ty = VD->getType();
  
  // Get the number of lexical arguments, for semantic checks below.
  int NumArguments = -1;
  if (AnyFunctionType *FT = dyn_cast<AnyFunctionType>(Ty))
    if (TupleType *TT = dyn_cast<TupleType>(FT->getInput()))
      NumArguments = TT->getFields().size();

  bool isOperator = VD->isOperator();

  // Operators must be declared with 'func', not 'var'.
  if (isOperator) {
    if (!isa<FuncDecl>(VD)) {
      TC.diagnose(VD->getStartLoc(), diag::operator_not_func);
      // FIXME: Set the 'isError' bit on the decl.
      return;
    }
  
    if (NumArguments == 0 || NumArguments > 2) {
      TC.diagnose(VD->getStartLoc(), diag::invalid_arg_count_for_operator);
      VD->getMutableAttrs().Infix = InfixData();
      // FIXME: Set the 'isError' bit on the decl.
      return;
    }

    // The unary operator '&' cannot be overloaded.  In an expression,
    // the parser never interprets this as a normal unary operator
    // anyway.
    if (NumArguments == 1 && VD->getName().str() == "&") {
      TC.diagnose(VD->getStartLoc(), diag::custom_operator_addressof);
      return;
    }
  }
  
  if (Attrs.isInfix()) {
    // Only operator functions can be infix.
    if (!isOperator) {
      TC.diagnose(VD->getStartLoc(), diag::infix_not_an_operator);
      VD->getMutableAttrs().Infix = InfixData();
      // FIXME: Set the 'isError' bit on the decl.
      return;
    }

    // Only binary operators can be infix.
    if (NumArguments != 2) {
      TC.diagnose(Attrs.LSquareLoc, diag::invalid_infix_left_input);
      VD->getMutableAttrs().Infix = InfixData();
      // FIXME: Set the 'isError' bit on the decl.
      return;
    }
  }

  if (Attrs.isPostfix()) {
    // Only operator functions can be postfix.
    if (!isOperator) {
      TC.diagnose(VD->getStartLoc(), diag::postfix_not_an_operator);
      VD->getMutableAttrs().Postfix = false;
      // FIXME: Set the 'isError' bit on the decl.
      return;
    }

    // Only unary operators can be postfix.
    if (NumArguments != 1) {
      TC.diagnose(VD->getStartLoc(), diag::invalid_postfix_input);
      VD->getMutableAttrs().Postfix = false;
      // FIXME: Set the 'isError' bit on the decl.
      return;
    }
  }

  if (Attrs.isAssignment()) {
    // Only function declarations can be assignments.
    if (!isa<FuncDecl>(VD) || !VD->isOperator()) {
      TC.diagnose(VD->getStartLoc(), diag::invalid_decl_attribute,"assignment");
      VD->getMutableAttrs().Assignment = false;
    } else if (NumArguments < 1) {
      TC.diagnose(VD->getStartLoc(), diag::assignment_without_byref);
      VD->getMutableAttrs().Assignment = false;
    } else {
      auto FT = VD->getType()->castTo<AnyFunctionType>();
      Type ParamType = FT->getInput();
      TupleType *ParamTT = ParamType->getAs<TupleType>();
      if (ParamTT)
        ParamType = ParamTT->getElementType(0);
      
      if (!ParamType->is<LValueType>()) {
        TC.diagnose(VD->getStartLoc(), diag::assignment_without_byref);
        VD->getMutableAttrs().Assignment = false;
      } else if (!FT->getResult()->isEqual(TupleType::getEmpty(TC.Context))) {
        TC.diagnose(VD->getStartLoc(), diag::assignment_nonvoid,
                    FT->getResult());
      }
    }
  }

  if (Attrs.isConversion()) {
    // Only instance members with no non-defaulted parameters can be
    // conversions.
    if (!isa<FuncDecl>(VD) || !VD->isInstanceMember()) {
      TC.diagnose(VD->getStartLoc(), diag::conversion_not_instance_method,
                  VD->getName());
      VD->getMutableAttrs().Conversion = false;
    } else if (!VD->getType()->is<ErrorType>()) {
      AnyFunctionType *BoundMethodTy
        = VD->getType()->castTo<AnyFunctionType>()->getResult()
            ->castTo<AnyFunctionType>();
      
      bool AcceptsEmptyParamList = false;
      Type InputTy = BoundMethodTy->getInput();
      if (const TupleType *Tuple = InputTy->getAs<TupleType>()) {
        bool AllDefaulted = true;
        for (auto Elt : Tuple->getFields()) {
          if (!Elt.hasInit()) {
            AllDefaulted = false;
            break;
          }
        }
        
        AcceptsEmptyParamList = AllDefaulted;
      }
      
      if (!AcceptsEmptyParamList) {
        TC.diagnose(VD->getStartLoc(), diag::conversion_params,
                    VD->getName());
        VD->getMutableAttrs().Conversion = false;
      }
    }
  }
  
  if (VD->isOperator() && !VD->getAttrs().isInfix() && NumArguments != 1) {
    // If this declaration is defined in the translation unit, check whether
    // there are any other operators in this scope with the same name that are
    // infix. If so, inherit that infix.
    // FIXME: This is a hack in so many ways. We may eventually want to separate
    // the declaration of an operator name + precedence from a new operator
    // function, or at the very least check the consistency of operator
    // associativity and precedence within a given scope.
    if (TranslationUnit *TU = dyn_cast<TranslationUnit>(VD->getDeclContext())) {
      // Look in the translation unit.
      for (Decl *D : TU->Decls) {
        if (ValueDecl *Existing = dyn_cast<ValueDecl>(D)) {
          if (Existing->getName() == VD->getName() &&
              Existing->getAttrs().isInfix()) {
            VD->getMutableAttrs().Infix = Existing->getAttrs().Infix;
            break;
          }
        }
      }
      
      // Look in imported modules.
      if (!VD->getAttrs().isInfix()) {
        for (auto &ModPath : TU->getImportedModules()) {
          if (Module *Mod = ModPath.second) {
            SmallVector<ValueDecl *, 4> Found;
            Mod->lookupValue(Module::AccessPathTy(), VD->getName(),
                             NLKind::QualifiedLookup, Found);
            for (ValueDecl *Existing : Found) {
              if (Existing->getName() == VD->getName() &&
                  Existing->getAttrs().isInfix()) {
                VD->getMutableAttrs().Infix = Existing->getAttrs().Infix;
                break;              
              }

            if (VD->getAttrs().isInfix())
              break;
            }
          }
        }
      }
    }
    
    if (!VD->getAttrs().isInfix())
      TC.diagnose(VD->getStartLoc(), diag::binops_infix_left);
  }

  if (Attrs.isByref()) {
    TC.diagnose(VD->getStartLoc(), diag::invalid_decl_attribute, "byref");
    VD->getMutableAttrs().Byref = false;
  }

  if (Attrs.isAutoClosure()) {
    TC.diagnose(VD->getStartLoc(), diag::invalid_decl_attribute, "auto_closure");
    VD->getMutableAttrs().AutoClosure = false;
  }  
}
