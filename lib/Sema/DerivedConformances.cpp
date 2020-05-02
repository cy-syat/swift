//===--- DerivedConformances.cpp - Derived conformance utilities ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Types.h"
#include "swift/ClangImporter/ClangModule.h"
#include "DerivedConformances.h"

using namespace swift;

DerivedConformance::DerivedConformance(ASTContext &ctx, Decl *conformanceDecl,
                                       NominalTypeDecl *nominal,
                                       ProtocolDecl *protocol)
    : Context(ctx), ConformanceDecl(conformanceDecl), Nominal(nominal),
      Protocol(protocol) {
  assert(getConformanceContext()->getSelfNominalTypeDecl() == nominal);
}

DeclContext *DerivedConformance::getConformanceContext() const {
  return cast<DeclContext>(ConformanceDecl);
}

void DerivedConformance::addMembersToConformanceContext(
    ArrayRef<Decl *> children) {
  auto IDC = cast<IterableDeclContext>(ConformanceDecl);
  auto *SF = ConformanceDecl->getDeclContext()->getParentSourceFile();
  for (auto child : children) {
    IDC->addMember(child);
    if (SF)
      SF->SynthesizedDecls.push_back(child);
  }
}

Type DerivedConformance::getProtocolType() const {
  return Protocol->getDeclaredType();
}

bool DerivedConformance::derivesProtocolConformance(DeclContext *DC,
                                                    NominalTypeDecl *Nominal,
                                                    ProtocolDecl *Protocol) {
  const auto derivableKind = Protocol->getKnownDerivableProtocolKind();
  if (!derivableKind)
    return false;

  // When the necessary requirements are met, the conformance to OptionSet
  // is serendipitously derived via memberwise initializer synthesis.
  if (*derivableKind == KnownDerivableProtocolKind::OptionSet) {
    return false;
  }

  if (*derivableKind == KnownDerivableProtocolKind::Hashable) {
    // We can always complete a partial Hashable implementation, and we can
    // synthesize a full Hashable implementation for structs and enums with
    // Hashable components.
    return canDeriveHashable(Nominal);
  }

  if (*derivableKind == KnownDerivableProtocolKind::AdditiveArithmetic)
    return canDeriveAdditiveArithmetic(Nominal, DC);

  if (*derivableKind == KnownDerivableProtocolKind::Differentiable)
    return canDeriveDifferentiable(Nominal, DC);

  // SWIFT_ENABLE_TENSORFLOW
  if (*derivableKind == KnownDerivableProtocolKind::PointwiseMultiplicative)
    return canDerivePointwiseMultiplicative(Nominal, DC);

  if (*derivableKind == KnownDerivableProtocolKind::ElementaryFunctions)
    return canDeriveElementaryFunctions(Nominal, DC);

  if (*derivableKind == KnownDerivableProtocolKind::KeyPathIterable)
    return canDeriveKeyPathIterable(Nominal);

  if (*derivableKind == KnownDerivableProtocolKind::TensorArrayProtocol)
    return canDeriveTensorArrayProtocol(Nominal, DC);

  if (*derivableKind == KnownDerivableProtocolKind::TensorGroup)
    return canDeriveTensorGroup(Nominal, DC);

  if (*derivableKind == KnownDerivableProtocolKind::VectorProtocol)
    return canDeriveVectorProtocol(Nominal, DC);

  if (*derivableKind == KnownDerivableProtocolKind::EuclideanDifferentiable)
    return canDeriveEuclideanDifferentiable(Nominal, DC);
  // SWIFT_ENABLE_TENSORFLOW END

  if (auto *enumDecl = dyn_cast<EnumDecl>(Nominal)) {
    switch (*derivableKind) {
        // The presence of a raw type is an explicit declaration that
        // the compiler should derive a RawRepresentable conformance.
      case KnownDerivableProtocolKind::RawRepresentable:
        return canDeriveRawRepresentable(DC, Nominal);

        // Enums without associated values can implicitly derive Equatable
        // conformance.
      case KnownDerivableProtocolKind::Equatable:
        return canDeriveEquatable(DC, Nominal);
      
      case KnownDerivableProtocolKind::Comparable:
        return !enumDecl->hasPotentiallyUnavailableCaseValue()
            && canDeriveComparable(DC, enumDecl); 

        // "Simple" enums without availability attributes can explicitly derive
        // a CaseIterable conformance.
        //
        // FIXME: Lift the availability restriction.
      case KnownDerivableProtocolKind::CaseIterable:
        return !enumDecl->hasPotentiallyUnavailableCaseValue()
            && enumDecl->hasOnlyCasesWithoutAssociatedValues();

        // @objc enums can explicitly derive their _BridgedNSError conformance.
      case KnownDerivableProtocolKind::BridgedNSError:
        return enumDecl->isObjC() && enumDecl->hasCases()
            && enumDecl->hasOnlyCasesWithoutAssociatedValues();

        // Enums without associated values and enums with a raw type of String
        // or Int can explicitly derive CodingKey conformance.
      case KnownDerivableProtocolKind::CodingKey: {
        Type rawType = enumDecl->getRawType();
        if (rawType) {
          auto parentDC = enumDecl->getDeclContext();
          ASTContext &C = parentDC->getASTContext();

          auto nominal = rawType->getAnyNominal();
          return nominal == C.getStringDecl() || nominal == C.getIntDecl();
        }

        // hasOnlyCasesWithoutAssociatedValues will return true for empty enums;
        // empty enums are allowed to conform as well.
        return enumDecl->hasOnlyCasesWithoutAssociatedValues();
      }

      default:
        return false;
    }
  } else if (isa<StructDecl>(Nominal) || isa<ClassDecl>(Nominal)) {
    // Structs and classes can explicitly derive Encodable and Decodable
    // conformance (explicitly meaning we can synthesize an implementation if
    // a type conforms manually).
    if (*derivableKind == KnownDerivableProtocolKind::Encodable ||
        *derivableKind == KnownDerivableProtocolKind::Decodable) {
      // FIXME: This is not actually correct. We cannot promise to always
      // provide a witness here for all structs and classes. Unfortunately,
      // figuring out whether this is actually possible requires much more
      // context -- a TypeChecker and the parent decl context at least -- and is
      // tightly coupled to the logic within DerivedConformance.
      // This unfortunately means that we expect a witness even if one will not
      // be produced, which requires DerivedConformance::deriveCodable to output
      // its own diagnostics.
      return true;
    }

    // Structs can explicitly derive Equatable conformance.
    if (isa<StructDecl>(Nominal)) {
      switch (*derivableKind) {
        case KnownDerivableProtocolKind::Equatable:
          return canDeriveEquatable(DC, Nominal);
        default:
          return false;
      }
    }
  }
  return false;
}

void DerivedConformance::tryDiagnoseFailedDerivation(DeclContext *DC,
                                                     NominalTypeDecl *nominal,
                                                     ProtocolDecl *protocol) {
  auto knownProtocol = protocol->getKnownProtocolKind();
  if (!knownProtocol)
    return;
  
  // Comparable on eligible type kinds should never fail
   
  if (*knownProtocol == KnownProtocolKind::Equatable) {
    tryDiagnoseFailedEquatableDerivation(DC, nominal);
  }

  if (*knownProtocol == KnownProtocolKind::Hashable) {
    tryDiagnoseFailedHashableDerivation(DC, nominal);
  }
}

ValueDecl *DerivedConformance::getDerivableRequirement(NominalTypeDecl *nominal,
                                                       ValueDecl *requirement) {
  // Note: whenever you update this function, also update
  // TypeChecker::deriveProtocolRequirement.
  ASTContext &ctx = nominal->getASTContext();
  const auto name = requirement->getName();

  // Local function that retrieves the requirement with the same name as
  // the provided requirement, but within the given known protocol.
  // SWIFT_ENABLE_TENSORFLOW
  auto getRequirement = [&](KnownProtocolKind kind,
                            llvm::function_ref<bool(ValueDecl *)> filter =
                                nullptr) -> ValueDecl * {
    // Dig out the protocol.
    auto proto = ctx.getProtocol(kind);
    if (!proto) return nullptr;

    auto conformance = nominal->getParentModule()->lookupConformance(
        nominal->getDeclaredInterfaceType(), proto);
    if (conformance) {
      auto DC = conformance.getConcrete()->getDeclContext();
      // Check whether this nominal type derives conformances to the protocol.
      if (!DerivedConformance::derivesProtocolConformance(DC, nominal, proto))
        return nullptr;
    }

    // Retrieve the requirement.
    // SWIFT_ENABLE_TENSORFLOW
    // Filter requirements, if `filter` function is specified.
    if (filter) {
      auto results = proto->lookupDirect(name);
      llvm::erase_if(results, [&](ValueDecl *v) {
        return !isa<ProtocolDecl>(v->getDeclContext()) ||
               !v->isProtocolRequirement() || !filter(v);
      });
      return results.empty() ? nullptr : results.front();
    }
    // SWIFT_ENABLE_TENSORFLOW END
    return proto->getSingleRequirement(name);
  };

  // Properties.
  if (isa<VarDecl>(requirement)) {
    // RawRepresentable.rawValue
    if (name.isSimpleName(ctx.Id_rawValue))
      return getRequirement(KnownProtocolKind::RawRepresentable);

    // Hashable.hashValue
    if (name.isSimpleName(ctx.Id_hashValue))
      return getRequirement(KnownProtocolKind::Hashable);

    // CaseIterable.allValues
    if (name.isSimpleName(ctx.Id_allCases))
      return getRequirement(KnownProtocolKind::CaseIterable);

    // _BridgedNSError._nsErrorDomain
    if (name.isSimpleName(ctx.Id_nsErrorDomain))
      return getRequirement(KnownProtocolKind::BridgedNSError);

    // CodingKey.stringValue
    if (name.isSimpleName(ctx.Id_stringValue))
      return getRequirement(KnownProtocolKind::CodingKey);

    // CodingKey.intValue
    if (name.isSimpleName(ctx.Id_intValue))
      return getRequirement(KnownProtocolKind::CodingKey);

    // AdditiveArithmetic.zero
    if (name.isSimpleName(ctx.Id_zero))
      return getRequirement(KnownProtocolKind::AdditiveArithmetic);

    // SWIFT_ENABLE_TENSORFLOW
    // EuclideanDifferentiable.differentiableVectorView
    if (name.isSimpleName(ctx.Id_differentiableVectorView))
      return getRequirement(KnownProtocolKind::EuclideanDifferentiable);

    // PointwiseMultiplicative.one
    if (name.isSimpleName(ctx.Id_one))
      return getRequirement(KnownProtocolKind::PointwiseMultiplicative);

    // PointwiseMultiplicative.reciprocal
    if (name.isSimpleName(ctx.Id_reciprocal))
      return getRequirement(KnownProtocolKind::PointwiseMultiplicative);

    // KeyPathIterable.allKeyPaths
    if (name.isSimpleName(ctx.Id_allKeyPaths))
      return getRequirement(KnownProtocolKind::KeyPathIterable);

    // TensorArrayProtocol._tensorHandleCount
    if (name.isSimpleName(ctx.Id_tensorHandleCount))
      return getRequirement(KnownProtocolKind::TensorArrayProtocol);

    // TensorArrayProtocol._typeList
    if (name.isSimpleName(ctx.Id_typeList) && !requirement->isStatic())
      return getRequirement(KnownProtocolKind::TensorArrayProtocol);

    // TensorGroup._typeList
    if (name.isSimpleName(ctx.Id_typeList))
      return getRequirement(KnownProtocolKind::TensorGroup);
    // SWIFT_ENABLE_TENSORFLOW END

    return nullptr;
  }

  // Functions.
  if (auto func = dyn_cast<FuncDecl>(requirement)) {
    if (func->isOperator() && name.getBaseName() == "<")
      return getRequirement(KnownProtocolKind::Comparable);
    
    if (func->isOperator() && name.getBaseName() == "==")
      return getRequirement(KnownProtocolKind::Equatable);

    // AdditiveArithmetic.+
    // AdditiveArithmetic.-
    if (func->isOperator() && name.getArgumentNames().size() == 2 &&
        (name.getBaseName() == "+" || name.getBaseName() == "-")) {
      return getRequirement(KnownProtocolKind::AdditiveArithmetic);
    }

    // Differentiable.move(along:)
    if (name.isCompoundName() && name.getBaseName() == ctx.Id_move) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 1 && argumentNames[0] == ctx.Id_along)
        return getRequirement(KnownProtocolKind::Differentiable);
    }

    // Encodable.encode(to: Encoder)
    if (name.isCompoundName() && name.getBaseName() == ctx.Id_encode) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 1 && argumentNames[0] == ctx.Id_to)
        return getRequirement(KnownProtocolKind::Encodable);
    }

    // Hashable.hash(into: inout Hasher)
    if (name.isCompoundName() && name.getBaseName() == ctx.Id_hash) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 1 && argumentNames[0] == ctx.Id_into)
        return getRequirement(KnownProtocolKind::Hashable);
    }

    // SWIFT_ENABLE_TENSORFLOW
    // AdditiveArithmetic.+
    // AdditiveArithmetic.-
    if (func->isOperator() && (name.getBaseName() == "+" ||
                               name.getBaseName() == "-")) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 2)
        return getRequirement(KnownProtocolKind::AdditiveArithmetic);
    }

    // SWIFT_ENABLE_TENSORFLOW
    // PointwiseMultiplicative.(.*)
    if (func->isOperator() && name.getBaseName() == ".*") {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 2)
        return getRequirement(KnownProtocolKind::PointwiseMultiplicative);
    }

    // SWIFT_ENABLE_TENSORFLOW
    // ElementaryFunctions requirements
    if (name.isCompoundName()) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 1 && (false
#define ELEMENTARY_FUNCTION_UNARY(ID, NAME) || name.getBaseName() == NAME
#include "DerivedConformanceElementaryFunctions.def"
#undef ELEMENTARY_FUNCTION_UNARY
                                        )) {
        return getRequirement(KnownProtocolKind::ElementaryFunctions);
      }
      if (argumentNames.size() == 2) {
        if (name.getBaseName() == "root")
          return getRequirement(KnownProtocolKind::ElementaryFunctions);
        if (name.getBaseName() == "pow") {
          return getRequirement(
              KnownProtocolKind::ElementaryFunctions,
              [&](ValueDecl *v) {
                auto *funcDecl = dyn_cast<FuncDecl>(v);
                if (!funcDecl)
                  return false;
                return funcDecl->getParameters()->get(1)->getName() ==
                       func->getParameters()->get(1)->getName();
              });
        }
      }
    }

    // SWIFT_ENABLE_TENSORFLOW
    // VectorProtocol.scaled(by:)
    if (name.isCompoundName() && name.getBaseName() == ctx.Id_scaled) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 1 &&
          argumentNames[0] == ctx.getIdentifier("by"))
        return getRequirement(KnownProtocolKind::VectorProtocol);
    }

    // SWIFT_ENABLE_TENSORFLOW
    // VectorProtocol.adding(_:)
    // VectorProtocol.subtracting(_:)
    if (name.isCompoundName() &&
        (name.getBaseName() == ctx.Id_adding ||
         name.getBaseName() == ctx.Id_subtracting)) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 1 && argumentNames[0].empty())
        return getRequirement(KnownProtocolKind::VectorProtocol);
    }

    // SWIFT_ENABLE_TENSORFLOW
    // TensorArrayProtocol._unpackTensorHandles(into:)
    if (name.isCompoundName() &&
        name.getBaseName() == ctx.Id_unpackTensorHandles) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 1 &&
          argumentNames[0] == ctx.getIdentifier("into")) {
        return getRequirement(KnownProtocolKind::TensorArrayProtocol);
      }
    }

    // SWIFT_ENABLE_TENSORFLOW
    // Differentiable.move(along:)
    if (name.isCompoundName() &&
        name.getBaseName() == ctx.Id_move) {
      auto argumentNames = name.getArgumentNames();
      if (argumentNames.size() == 1 &&
          argumentNames[0] == ctx.getIdentifier("along")) {
        return getRequirement(KnownProtocolKind::Differentiable);
      }
    }

    return nullptr;
  }

  // Initializers.
  if (auto ctor = dyn_cast<ConstructorDecl>(requirement)) {
    auto argumentNames = name.getArgumentNames();
    if (argumentNames.size() == 1) {
      if (argumentNames[0] == ctx.Id_rawValue)
        return getRequirement(KnownProtocolKind::RawRepresentable);

      // CodingKey.init?(stringValue:), CodingKey.init?(intValue:)
      if (ctor->isFailable() &&
          !ctor->isImplicitlyUnwrappedOptional() &&
          (argumentNames[0] == ctx.Id_stringValue ||
           argumentNames[0] == ctx.Id_intValue))
        return getRequirement(KnownProtocolKind::CodingKey);

      // Decodable.init(from: Decoder)
      if (argumentNames[0] == ctx.Id_from)
        return getRequirement(KnownProtocolKind::Decodable);

      // SWIFT_ENABLE_TENSORFLOW
      // TensorGroup.init(_owning:)
      if (argumentNames[0] == ctx.getIdentifier("_owning")) {
        return getRequirement(KnownProtocolKind::TensorGroup);
      }
    } else if (argumentNames.size() == 2) {
      // SWIFT_ENABLE_TENSORFLOW
      // TensorArrayProtocol.init(_owning:count)
      if (argumentNames[0] == ctx.getIdentifier("_owning") &&
          argumentNames[1] == ctx.getIdentifier("count")) {
        return getRequirement(KnownProtocolKind::TensorArrayProtocol);
      }
    }

    return nullptr;
  }

  // Associated types.
  if (isa<AssociatedTypeDecl>(requirement)) {
    // RawRepresentable.RawValue
    if (name.isSimpleName(ctx.Id_RawValue))
      return getRequirement(KnownProtocolKind::RawRepresentable);

    // CaseIterable.AllCases
    if (name.isSimpleName(ctx.Id_AllCases))
      return getRequirement(KnownProtocolKind::CaseIterable);

    // Differentiable.TangentVector
    if (name.isSimpleName(ctx.Id_TangentVector))
      return getRequirement(KnownProtocolKind::Differentiable);

    // SWIFT_ENABLE_TENSORFLOW
    // KeyPathIterable.AllKeyPaths
    if (name.isSimpleName(ctx.Id_AllKeyPaths))
      return getRequirement(KnownProtocolKind::KeyPathIterable);

    // VectorProtocol.VectorSpaceScalar
    if (name.isSimpleName(ctx.Id_VectorSpaceScalar))
      return getRequirement(KnownProtocolKind::VectorProtocol);
    // SWIFT_ENABLE_TENSORFLOW END

    return nullptr;
  }

  return nullptr;
}

DeclRefExpr *
DerivedConformance::createSelfDeclRef(AbstractFunctionDecl *fn) {
  ASTContext &C = fn->getASTContext();

  auto selfDecl = fn->getImplicitSelfDecl();
  return new (C) DeclRefExpr(selfDecl, DeclNameLoc(), /*implicit*/true);
}

AccessorDecl *DerivedConformance::
addGetterToReadOnlyDerivedProperty(VarDecl *property,
                                   Type propertyContextType) {
  auto getter =
    declareDerivedPropertyGetter(property, propertyContextType);

  property->setImplInfo(StorageImplInfo::getImmutableComputed());
  property->setAccessors(SourceLoc(), {getter}, SourceLoc());

  return getter;
}

AccessorDecl *
DerivedConformance::declareDerivedPropertyGetter(VarDecl *property,
                                                 Type propertyContextType) {
  auto &C = property->getASTContext();
  auto parentDC = property->getDeclContext();
  ParameterList *params = ParameterList::createEmpty(C);

  Type propertyInterfaceType = property->getInterfaceType();
  
  auto getterDecl = AccessorDecl::create(C,
    /*FuncLoc=*/SourceLoc(), /*AccessorKeywordLoc=*/SourceLoc(),
    AccessorKind::Get, property,
    /*StaticLoc=*/SourceLoc(), StaticSpellingKind::None,
    /*Throws=*/false, /*ThrowsLoc=*/SourceLoc(),
    /*GenericParams=*/nullptr, params,
    TypeLoc::withoutLoc(propertyInterfaceType), parentDC);
  getterDecl->setImplicit();
  getterDecl->setIsTransparent(false);

  getterDecl->copyFormalAccessFrom(property);


  return getterDecl;
}

// SWIFT_ENABLE_TENSORFLOW
std::pair<AccessorDecl *, AccessorDecl *>
DerivedConformance::addGetterAndSetterToMutableDerivedProperty(
    VarDecl *property, Type propertyContextType) {
  auto *getter = declareDerivedPropertyGetter(property, propertyContextType);
  auto *setter = declareDerivedPropertySetter(property, propertyContextType);
  property->setImplInfo(StorageImplInfo::getMutableComputed());
  property->setAccessors(SourceLoc(), {getter, setter}, SourceLoc());
  return std::make_pair(getter, setter);
}
// SWIFT_ENABLE_TENSORFLOW END

// SWIFT_ENABLE_TENSORFLOW
AccessorDecl *
DerivedConformance::declareDerivedPropertySetter(VarDecl *property,
                                                 Type propertyContextType) {
  bool isStatic = property->isStatic();
  bool isFinal = property->isFinal();

  auto &C = property->getASTContext();
  auto parentDC = property->getDeclContext();

  auto propertyInterfaceType = property->getInterfaceType();
  auto propertyParam = new (C) ParamDecl(SourceLoc(), SourceLoc(), Identifier(),
              property->getLoc(), C.getIdentifier("newValue"), parentDC);
  propertyParam->setSpecifier(ParamDecl::Specifier::Default);
  propertyParam->setInterfaceType(propertyInterfaceType);

  ParameterList *params = ParameterList::create(C, propertyParam);

  auto setterDecl = AccessorDecl::create(C,
    /*FuncLoc*/ SourceLoc(), /*AccessorKeywordLoc*/ SourceLoc(),
    AccessorKind::Set, property, /*StaticLoc*/ SourceLoc(),
    StaticSpellingKind::None, /*Throws*/ false, /*ThrowsLoc*/ SourceLoc(),
    /*GenericParams*/ nullptr, params, TypeLoc(), parentDC);
  setterDecl->setImplicit();
  setterDecl->setStatic(isStatic);
  // Set mutating if parent is not a class.
  if (!parentDC->getSelfClassDecl())
    setterDecl->setSelfAccessKind(SelfAccessKind::Mutating);

  // If this is supposed to be a final method, mark it as such.
  assert(isFinal || !parentDC->getSelfClassDecl());
  if (isFinal && parentDC->getSelfClassDecl() &&
      !setterDecl->isFinal())
    setterDecl->getAttrs().add(new (C) FinalAttr(/*Implicit*/ true));

  // Compute the interface type of the setter.
  setterDecl->setGenericSignature(parentDC->getGenericSignatureOfContext());
  setterDecl->copyFormalAccessFrom(property);

  return setterDecl;
}

std::pair<VarDecl *, PatternBindingDecl *>
DerivedConformance::declareDerivedProperty(Identifier name,
                                           Type propertyInterfaceType,
                                           Type propertyContextType,
                                           bool isStatic, bool isFinal) {
  auto parentDC = getConformanceContext();

  VarDecl *propDecl = new (Context)
      VarDecl(/*IsStatic*/ isStatic, VarDecl::Introducer::Var,
              /*IsCaptureList*/ false, SourceLoc(), name, parentDC);
  // SWIFT_ENABLE_TENSORFLOW
  // TODO: Upstream this change to master.
  if (isFinal && parentDC->getSelfClassDecl())
    propDecl->getAttrs().add(new (Context) FinalAttr(/*Implicit*/ true));
  propDecl->setImplicit();
  propDecl->copyFormalAccessFrom(Nominal, /*sourceIsParentContext*/ true);
  propDecl->setInterfaceType(propertyInterfaceType);

  Pattern *propPat = NamedPattern::createImplicit(Context, propDecl);
  propPat->setType(propertyContextType);

  propPat = TypedPattern::createImplicit(Context, propPat, propertyContextType);
  propPat->setType(propertyContextType);

  auto *pbDecl = PatternBindingDecl::createImplicit(
      Context, StaticSpellingKind::None, propPat, /*InitExpr*/ nullptr,
      parentDC);
  return {propDecl, pbDecl};
}

bool DerivedConformance::checkAndDiagnoseDisallowedContext(
    ValueDecl *synthesizing) const {
  // In general, conformances can't be synthesized in extensions across files;
  // but we have to allow it as a special case for Equatable and Hashable on
  // enums with no associated values to preserve source compatibility.
  bool allowCrossfileExtensions = false;
  if (Protocol->isSpecificProtocol(KnownProtocolKind::Equatable) ||
      Protocol->isSpecificProtocol(KnownProtocolKind::Hashable)) {
    auto ED = dyn_cast<EnumDecl>(Nominal);
    allowCrossfileExtensions = ED && ED->hasOnlyCasesWithoutAssociatedValues();
  }

  if (!allowCrossfileExtensions &&
      Nominal->getModuleScopeContext() !=
          getConformanceContext()->getModuleScopeContext()) {
    ConformanceDecl->diagnose(diag::cannot_synthesize_in_crossfile_extension,
                              getProtocolType());
    Nominal->diagnose(diag::kind_declared_here, DescriptiveDeclKind::Type);
    return true;
  }

  // A non-final class can't have an protocol-witnesss initializer in an
  // extension.
  if (auto CD = dyn_cast<ClassDecl>(Nominal)) {
    if (!CD->isFinal() && isa<ConstructorDecl>(synthesizing) &&
        isa<ExtensionDecl>(ConformanceDecl)) {
      ConformanceDecl->diagnose(
          diag::cannot_synthesize_init_in_extension_of_nonfinal,
          getProtocolType(), synthesizing->getName());
      return true;
    }
  }

  return false;
}

/// Returns a generated guard statement that checks whether the given lhs and
/// rhs expressions are equal. If not equal, the else block for the guard
/// returns `guardReturnValue`.
/// \p C The AST context.
/// \p lhsExpr The first expression to compare for equality.
/// \p rhsExpr The second expression to compare for equality.
/// \p guardReturnValue The expression to return if the two sides are not equal 
GuardStmt *DerivedConformance::returnIfNotEqualGuard(ASTContext &C,
                                        Expr *lhsExpr,
                                        Expr *rhsExpr, 
                                        Expr *guardReturnValue) {
  SmallVector<StmtConditionElement, 1> conditions;
  SmallVector<ASTNode, 1> statements;
  
  auto returnStmt = new (C) ReturnStmt(SourceLoc(), guardReturnValue);
  statements.push_back(returnStmt);

  // Next, generate the condition being checked.
  // lhs == rhs
  auto cmpFuncExpr = new (C) UnresolvedDeclRefExpr(
    DeclNameRef(C.Id_EqualsOperator), DeclRefKind::BinaryOperator,
    DeclNameLoc());
  auto cmpArgsTuple = TupleExpr::create(C, SourceLoc(),
                                        { lhsExpr, rhsExpr },
                                        { }, { }, SourceLoc(),
                                        /*HasTrailingClosure*/false,
                                        /*Implicit*/true);
  auto cmpExpr = new (C) BinaryExpr(cmpFuncExpr, cmpArgsTuple,
                                    /*Implicit*/true);
  conditions.emplace_back(cmpExpr);

  // Build and return the complete guard statement.
  // guard lhs == rhs else { return lhs < rhs }
  auto body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc());
  return new (C) GuardStmt(SourceLoc(), C.AllocateCopy(conditions), body);
}
/// Returns a generated guard statement that checks whether the given lhs and
/// rhs expressions are equal. If not equal, the else block for the guard
/// returns `false`.
/// \p C The AST context.
/// \p lhsExpr The first expression to compare for equality.
/// \p rhsExpr The second expression to compare for equality. 
GuardStmt *DerivedConformance::returnFalseIfNotEqualGuard(ASTContext &C,
                                        Expr *lhsExpr,
                                        Expr *rhsExpr) {
  // return false
  auto falseExpr = new (C) BooleanLiteralExpr(false, SourceLoc(), true);
  return returnIfNotEqualGuard(C, lhsExpr, rhsExpr, falseExpr);
}
/// Returns a generated guard statement that checks whether the given lhs and
/// rhs expressions are equal. If not equal, the else block for the guard
/// returns lhs < rhs.
/// \p C The AST context.
/// \p lhsExpr The first expression to compare for equality.
/// \p rhsExpr The second expression to compare for equality. 
GuardStmt *DerivedConformance::returnComparisonIfNotEqualGuard(ASTContext &C,
                                        Expr *lhsExpr,
                                        Expr *rhsExpr) {
  // return lhs < rhs
  auto ltFuncExpr = new (C) UnresolvedDeclRefExpr(
    DeclNameRef(C.Id_LessThanOperator), DeclRefKind::BinaryOperator,
    DeclNameLoc());
  auto ltArgsTuple = TupleExpr::create(C, SourceLoc(),
                                        { lhsExpr, rhsExpr },
                                        { }, { }, SourceLoc(),
                                        /*HasTrailingClosure*/false,
                                        /*Implicit*/true);
  auto ltExpr = new (C) BinaryExpr(ltFuncExpr, ltArgsTuple, /*Implicit*/true);
  return returnIfNotEqualGuard(C, lhsExpr, rhsExpr, ltExpr);
}

/// Build a type-checked integer literal.
static IntegerLiteralExpr *buildIntegerLiteral(ASTContext &C, unsigned index) {
  Type intType = C.getIntDecl()->getDeclaredType();

  auto literal = IntegerLiteralExpr::createFromUnsigned(C, index);
  literal->setType(intType);
  literal->setBuiltinInitializer(C.getIntBuiltinInitDecl(C.getIntDecl()));

  return literal;
}

/// Create AST statements which convert from an enum to an Int with a switch.
/// \p stmts The generated statements are appended to this vector.
/// \p parentDC Either an extension or the enum itself.
/// \p enumDecl The enum declaration.
/// \p enumVarDecl The enum input variable.
/// \p funcDecl The parent function.
/// \p indexName The name of the output variable.
/// \return A DeclRefExpr of the output variable (of type Int).
DeclRefExpr *DerivedConformance::convertEnumToIndex(SmallVectorImpl<ASTNode> &stmts,
                                       DeclContext *parentDC,
                                       EnumDecl *enumDecl,
                                       VarDecl *enumVarDecl,
                                       AbstractFunctionDecl *funcDecl,
                                       const char *indexName) {
  ASTContext &C = enumDecl->getASTContext();
  Type enumType = enumVarDecl->getType();
  Type intType = C.getIntDecl()->getDeclaredType();

  auto indexVar = new (C) VarDecl(/*IsStatic*/false, VarDecl::Introducer::Var,
                                  /*IsCaptureList*/false, SourceLoc(),
                                  C.getIdentifier(indexName),
                                  funcDecl);
  indexVar->setInterfaceType(intType);
  indexVar->setImplicit();

  // generate: var indexVar
  Pattern *indexPat = NamedPattern::createImplicit(C, indexVar);
  indexPat->setType(intType);
  indexPat = TypedPattern::createImplicit(C, indexPat, intType);
  indexPat->setType(intType);
  auto *indexBind = PatternBindingDecl::createImplicit(
      C, StaticSpellingKind::None, indexPat, /*InitExpr*/ nullptr, funcDecl);

  unsigned index = 0;
  SmallVector<ASTNode, 4> cases;
  for (auto elt : enumDecl->getAllElements()) {
    // generate: case .<Case>:
    auto pat = new (C) EnumElementPattern(TypeLoc::withoutLoc(enumType),
                                          SourceLoc(), DeclNameLoc(),
                                          DeclNameRef(), elt, nullptr);
    pat->setImplicit();
    pat->setType(enumType);

    auto labelItem = CaseLabelItem(pat);

    // generate: indexVar = <index>
    auto indexExpr = buildIntegerLiteral(C, index++);

    auto indexRef = new (C) DeclRefExpr(indexVar, DeclNameLoc(),
                                        /*implicit*/true,
                                        AccessSemantics::Ordinary,
                                        LValueType::get(intType));
    auto assignExpr = new (C) AssignExpr(indexRef, SourceLoc(),
                                         indexExpr, /*implicit*/ true);
    assignExpr->setType(TupleType::getEmpty(C));
    auto body = BraceStmt::create(C, SourceLoc(), ASTNode(assignExpr),
                                  SourceLoc());
    cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                     labelItem, SourceLoc(), SourceLoc(), body,
                                     /*case body vardecls*/ None));
  }

  // generate: switch enumVar { }
  auto enumRef = new (C) DeclRefExpr(enumVarDecl, DeclNameLoc(),
                                     /*implicit*/true,
                                     AccessSemantics::Ordinary,
                                     enumVarDecl->getType());
  auto switchStmt = SwitchStmt::create(LabeledStmtInfo(), SourceLoc(), enumRef,
                                       SourceLoc(), cases, SourceLoc(), C);

  stmts.push_back(indexBind);
  stmts.push_back(switchStmt);

  return new (C) DeclRefExpr(indexVar, DeclNameLoc(), /*implicit*/ true,
                             AccessSemantics::Ordinary, intType);
}

/// Returns the ParamDecl for each associated value of the given enum whose type
/// does not conform to a protocol
/// \p theEnum The enum whose elements and associated values should be checked.
/// \p protocol The protocol being requested.
/// \return The ParamDecl of each associated value whose type does not conform.
SmallVector<ParamDecl *, 4>
DerivedConformance::associatedValuesNotConformingToProtocol(DeclContext *DC, EnumDecl *theEnum,
                                        ProtocolDecl *protocol) {
  SmallVector<ParamDecl *, 4> nonconformingAssociatedValues;
  for (auto elt : theEnum->getAllElements()) {
    auto PL = elt->getParameterList();
    if (!PL)
      continue;

    for (auto param : *PL) {
      auto type = param->getInterfaceType();
      if (TypeChecker::conformsToProtocol(DC->mapTypeIntoContext(type),
                                          protocol, DC)
              .isInvalid()) {
        nonconformingAssociatedValues.push_back(param);
      }
    }
  }
  return nonconformingAssociatedValues;
}

/// Returns true if, for every element of the given enum, it either has no
/// associated values or all of them conform to a protocol.
/// \p theEnum The enum whose elements and associated values should be checked.
/// \p protocol The protocol being requested.
/// \return True if all associated values of all elements of the enum conform.
bool DerivedConformance::allAssociatedValuesConformToProtocol(DeclContext *DC,
                                                 EnumDecl *theEnum,
                                                 ProtocolDecl *protocol) {
  return associatedValuesNotConformingToProtocol(DC, theEnum, protocol).empty();
}

/// Returns the pattern used to match and bind the associated values (if any) of
/// an enum case.
/// \p enumElementDecl The enum element to match.
/// \p varPrefix The prefix character for variable names (e.g., a0, a1, ...).
/// \p varContext The context into which payload variables should be declared.
/// \p boundVars The array to which the pattern's variables will be appended.
Pattern*
DerivedConformance::enumElementPayloadSubpattern(EnumElementDecl *enumElementDecl,
                             char varPrefix, DeclContext *varContext,
                             SmallVectorImpl<VarDecl*> &boundVars) {
  auto parentDC = enumElementDecl->getDeclContext();
  ASTContext &C = parentDC->getASTContext();

  // No arguments, so no subpattern to match.
  if (!enumElementDecl->hasAssociatedValues())
    return nullptr;

  auto argumentType = enumElementDecl->getArgumentInterfaceType();
  if (auto tupleType = argumentType->getAs<TupleType>()) {
    // Either multiple (labeled or unlabeled) arguments, or one labeled
    // argument. Return a tuple pattern that matches the enum element in arity,
    // types, and labels. For example:
    // case a(x: Int) => (x: let a0)
    // case b(Int, String) => (let a0, let a1)
    SmallVector<TuplePatternElt, 4> elementPatterns;
    int index = 0;
    for (auto tupleElement : tupleType->getElements()) {
      auto payloadVar = indexedVarDecl(varPrefix, index++,
                                       tupleElement.getType(), varContext);
      boundVars.push_back(payloadVar);

      auto namedPattern = new (C) NamedPattern(payloadVar);
      namedPattern->setImplicit();
      auto letPattern = VarPattern::createImplicit(C, /*isLet*/ true,
                                                   namedPattern);
      elementPatterns.push_back(TuplePatternElt(tupleElement.getName(),
                                                SourceLoc(), letPattern));
    }

    auto pat = TuplePattern::createImplicit(C, elementPatterns);
    pat->setImplicit();
    return pat;
  }

  // Otherwise, a one-argument unlabeled payload. Return a paren pattern whose
  // underlying type is the same as the payload. For example:
  // case a(Int) => (let a0)
  auto underlyingType = argumentType->getWithoutParens();
  auto payloadVar = indexedVarDecl(varPrefix, 0, underlyingType, varContext);
  boundVars.push_back(payloadVar);

  auto namedPattern = new (C) NamedPattern(payloadVar);
  namedPattern->setImplicit();
  auto letPattern = new (C) VarPattern(SourceLoc(), /*isLet*/ true,
                                       namedPattern);
  return ParenPattern::createImplicit(C, letPattern);
}


/// Creates a named variable based on a prefix character and a numeric index.
/// \p prefixChar The prefix character for the variable's name.
/// \p index The numeric index to append to the variable's name.
/// \p type The type of the variable.
/// \p varContext The context of the variable.
/// \return A VarDecl named with the prefix and number.
VarDecl *DerivedConformance::indexedVarDecl(char prefixChar, int index, Type type,
                               DeclContext *varContext) {
  ASTContext &C = varContext->getASTContext();

  llvm::SmallString<8> indexVal;
  indexVal.append(1, prefixChar);
  APInt(32, index).toString(indexVal, 10, /*signed*/ false);
  auto indexStr = C.AllocateCopy(indexVal);
  auto indexStrRef = StringRef(indexStr.data(), indexStr.size());

  auto varDecl = new (C) VarDecl(/*IsStatic*/false, VarDecl::Introducer::Let,
                                 /*IsCaptureList*/true, SourceLoc(),
                                 C.getIdentifier(indexStrRef),
                                 varContext);
  varDecl->setInterfaceType(type);
  varDecl->setHasNonPatternBindingInit(true);
  return varDecl;
}
