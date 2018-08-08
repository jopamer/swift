//===--- TypeCheckType.cpp - Type Validation ------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements validation for Swift types, emitting semantic errors as
// appropriate and checking default initializer values.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "GenericTypeResolver.h"
#include "TypeCheckAvailability.h"
#include "TypeCheckProtocol.h"

#include "swift/Strings.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeLoc.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/StringExtras.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;

#define DEBUG_TYPE "TypeCheckType"

GenericTypeResolver::~GenericTypeResolver() { }

Type TypeChecker::getArraySliceType(SourceLoc loc, Type elementType) {
  if (!Context.getArrayDecl()) {
    diagnose(loc, diag::sugar_type_not_found, 0);
    return Type();
  }

  return ArraySliceType::get(elementType);
}

Type TypeChecker::getDictionaryType(SourceLoc loc, Type keyType, 
                                    Type valueType) {
  if (!Context.getDictionaryDecl()) {
    diagnose(loc, diag::sugar_type_not_found, 3);
    return Type();
  }

    return DictionaryType::get(keyType, valueType);
}

Type TypeChecker::getOptionalType(SourceLoc loc, Type elementType) {
  if (!Context.getOptionalDecl()) {
    diagnose(loc, diag::sugar_type_not_found, 1);
    return Type();
  }

  return OptionalType::get(elementType);
}

static Type getPointerType(TypeChecker &tc, SourceLoc loc, Type pointeeType,
                           PointerTypeKind kind) {
  auto pointerDecl = [&] {
    switch (kind) {
    case PTK_UnsafeMutableRawPointer:
    case PTK_UnsafeRawPointer:
      llvm_unreachable("these pointer types don't take arguments");
    case PTK_UnsafePointer:
      return tc.Context.getUnsafePointerDecl();
    case PTK_UnsafeMutablePointer:
      return tc.Context.getUnsafeMutablePointerDecl();
    case PTK_AutoreleasingUnsafeMutablePointer:
      return tc.Context.getAutoreleasingUnsafeMutablePointerDecl();
    }
    llvm_unreachable("bad kind");
  }();
  if (!pointerDecl) {
    tc.diagnose(loc, diag::pointer_type_not_found,
                kind == PTK_UnsafePointer ? 0 :
                kind == PTK_UnsafeMutablePointer ? 1 : 2);
    return Type();
  }

  tc.validateDecl(pointerDecl);
  if (pointerDecl->isInvalid())
    return Type();

  // TODO: validate generic signature?

  return BoundGenericType::get(pointerDecl, nullptr, pointeeType);
}

Type TypeChecker::getUnsafePointerType(SourceLoc loc, Type pointeeType) {
  return getPointerType(*this, loc, pointeeType, PTK_UnsafePointer);
}

Type TypeChecker::getUnsafeMutablePointerType(SourceLoc loc, Type pointeeType) {
  return getPointerType(*this, loc, pointeeType, PTK_UnsafeMutablePointer);
}

static Type getStdlibType(TypeChecker &TC, Type &cached, DeclContext *dc,
                          StringRef name) {
  if (cached.isNull()) {
    ModuleDecl *stdlib = TC.Context.getStdlibModule();
    LookupTypeResult lookup = TC.lookupMemberType(dc, ModuleType::get(stdlib),
                                                  TC.Context.getIdentifier(
                                                    name));
    if (lookup)
      cached = lookup.back().MemberType;
  }
  return cached;
}

Type TypeChecker::getStringType(DeclContext *dc) {
  return ::getStdlibType(*this, StringType, dc, "String");
}
Type TypeChecker::getSubstringType(DeclContext *dc) {
  return ::getStdlibType(*this, SubstringType, dc, "Substring");
}
Type TypeChecker::getIntType(DeclContext *dc) {
  return ::getStdlibType(*this, IntType, dc, "Int");
}
Type TypeChecker::getInt8Type(DeclContext *dc) {
  return ::getStdlibType(*this, Int8Type, dc, "Int8");
}
Type TypeChecker::getUInt8Type(DeclContext *dc) {
  return ::getStdlibType(*this, UInt8Type, dc, "UInt8");
}

/// Returns the maximum-sized builtin integer type.
Type TypeChecker::getMaxIntegerType(DeclContext *dc) {
  if (!MaxIntegerType.isNull())
    return MaxIntegerType;

  SmallVector<ValueDecl *, 1> lookupResults;
  getStdlibModule(dc)->lookupValue(/*AccessPath=*/{},
                                   Context.Id_MaxBuiltinIntegerType,
                                   NLKind::QualifiedLookup, lookupResults);
  if (lookupResults.size() != 1)
    return MaxIntegerType;

  auto *maxIntegerTypeDecl = dyn_cast<TypeAliasDecl>(lookupResults.front());
  if (!maxIntegerTypeDecl)
    return MaxIntegerType;

  validateDecl(maxIntegerTypeDecl);
  if (!maxIntegerTypeDecl->hasInterfaceType() ||
      !maxIntegerTypeDecl->getDeclaredInterfaceType()->is<BuiltinIntegerType>())
    return MaxIntegerType;

  MaxIntegerType = maxIntegerTypeDecl->getUnderlyingTypeLoc().getType();
  return MaxIntegerType;
}

/// Find the standard type of exceptions.
///
/// We call this the "exception type" to try to avoid confusion with
/// the AST's ErrorType node.
Type TypeChecker::getExceptionType(DeclContext *dc, SourceLoc loc) {
  if (NominalTypeDecl *decl = Context.getErrorDecl())
    return decl->getDeclaredType();

  // Not really sugar, but the actual diagnostic text is fine.
  diagnose(loc, diag::sugar_type_not_found, 4);
  return Type();
}

Type
TypeChecker::getDynamicBridgedThroughObjCClass(DeclContext *dc,
                                               Type dynamicType,
                                               Type valueType) {
  // We can only bridge from class or Objective-C existential types.
  if (!dynamicType->satisfiesClassConstraint())
    return Type();

  // If the value type cannot be bridged, we're done.
  if (!valueType->isPotentiallyBridgedValueType())
    return Type();

  return Context.getBridgedToObjC(dc, valueType);
}

Type TypeChecker::resolveTypeInContext(
       TypeDecl *typeDecl,
       DeclContext *foundDC,
       DeclContext *fromDC,
       TypeResolutionOptions options,
       bool isSpecialized,
       GenericTypeResolver *resolver) {
  // If we're just resolving the structure, the decl itself is all we need to
  // know: return the unbound generic type.
  if (options.contains(TypeResolutionFlags::ResolveStructure))
    return typeDecl->getDeclaredInterfaceType();

  GenericTypeToArchetypeResolver defaultResolver(fromDC);
  if (!resolver)
    resolver = &defaultResolver;

  // If we found a generic parameter, map to the archetype if there is one.
  if (auto genericParam = dyn_cast<GenericTypeParamDecl>(typeDecl)) {
    return resolver->mapTypeIntoContext(
      genericParam->getDeclaredInterfaceType());
  }

  // If we are referring to a type within its own context, and we have either
  // a generic type with no generic arguments or a non-generic type, use the
  // type within the context.
  if (auto nominalType = dyn_cast<NominalTypeDecl>(typeDecl)) {
    if (!isa<ProtocolDecl>(nominalType) &&
        (!nominalType->getGenericParams() || !isSpecialized)) {
      for (auto parentDC = fromDC;
           !parentDC->isModuleScopeContext();
           parentDC = parentDC->getParent()) {
        auto *parentNominal =
          parentDC->getAsNominalTypeOrNominalTypeExtensionContext();
        if (parentNominal == nominalType)
          return resolver->mapTypeIntoContext(
            parentDC->getSelfInterfaceType());
        if (isa<ExtensionDecl>(parentDC)) {
          auto *extendedType = parentNominal;
          while (extendedType != nullptr) {
            if (extendedType == nominalType)
              return resolver->mapTypeIntoContext(
                extendedType->getDeclaredInterfaceType());
            extendedType = extendedType->getParent()
              ->getAsNominalTypeOrNominalTypeExtensionContext();
          }
        }
      }
    }
  }

  // Simple case -- the type is not nested inside of another type.
  // However, it might be nested inside another generic context, so
  // we do want to write the type in terms of interface types or
  // context archetypes, depending on the resolver given to us.
  if (!typeDecl->getDeclContext()->isTypeContext()) {
    if (auto *aliasDecl = dyn_cast<TypeAliasDecl>(typeDecl)) {
      // For a generic typealias, return the unbound generic form of the type.
      if (aliasDecl->getGenericParams())
        return aliasDecl->getUnboundGenericType();

      // Otherwise, simply return the underlying type.
      return resolver->mapTypeIntoContext(
        aliasDecl->getDeclaredInterfaceType());
    }

    // When a nominal type used outside its context, return the unbound
    // generic form of the type.
    if (auto *nominalDecl = dyn_cast<NominalTypeDecl>(typeDecl))
      return nominalDecl->getDeclaredType();

    assert(isa<ModuleDecl>(typeDecl));
    return typeDecl->getDeclaredInterfaceType();
  }

  assert(foundDC);

  // selfType is the self type of the context, unless the
  // context is a protocol type, in which case we might have
  // to use the existential type or superclass bound as a
  // parent type instead.
  Type selfType;
  if (isa<NominalTypeDecl>(typeDecl) &&
      typeDecl->getDeclContext()->getAsProtocolOrProtocolExtensionContext()) {
    // When looking up a nominal type declaration inside of a
    // protocol extension, always use the nominal type and
    // not the protocol 'Self' type.
    if (!foundDC->getDeclaredInterfaceType())
      return ErrorType::get(Context);

    selfType = resolver->mapTypeIntoContext(
      foundDC->getDeclaredInterfaceType());
  } else {
    // Otherwise, we want the protocol 'Self' type for
    // substituting into alias types and associated types.
    selfType = resolver->mapTypeIntoContext(
      foundDC->getSelfInterfaceType());

    if (selfType->is<GenericTypeParamType>() &&
        typeDecl->getDeclContext()->getAsClassOrClassExtensionContext()) {
      // We found a member of a class from a protocol or protocol
      // extension.
      //
      // Get the superclass of the 'Self' type parameter.
      auto *sig = foundDC->getGenericSignatureOfContext();
      auto superclassType = sig->getSuperclassBound(selfType);
      assert(superclassType);
      selfType = superclassType;
    }
  }
  
  // Finally, substitute the base type into the member type.
  return substMemberTypeWithBase(fromDC->getParentModule(), typeDecl,
                                 selfType, resolver->usesArchetypes());
}

static TypeResolutionOptions
adjustOptionsForGenericArgs(TypeResolutionOptions options) {
  options -= TypeResolutionFlags::SILType;
  options -= TypeResolutionFlags::FunctionInput;
  options -= TypeResolutionFlags::TypeAliasUnderlyingType;
  options -= TypeResolutionFlags::AllowUnavailableProtocol;

  return options;
}

/// This function checks if a bound generic type is UnsafePointer<Void> or
/// UnsafeMutablePointer<Void>. For these two type representations, we should
/// warn users that they are deprecated and replace them with more handy
/// UnsafeRawPointer and UnsafeMutableRawPointer, respectively.
static bool isPointerToVoid(ASTContext &Ctx, Type Ty, bool &IsMutable) {
  if (Ty.isNull())
    return false;
  auto *BGT = Ty->getAs<BoundGenericType>();
  if (!BGT)
    return false;
  if (BGT->getDecl() != Ctx.getUnsafePointerDecl() &&
      BGT->getDecl() != Ctx.getUnsafeMutablePointerDecl())
    return false;
  IsMutable = BGT->getDecl() == Ctx.getUnsafeMutablePointerDecl();
  assert(BGT->getGenericArgs().size() == 1);
  return BGT->getGenericArgs().front()->isVoid();
}

Type TypeChecker::applyGenericArguments(Type type,
                                        SourceLoc loc, DeclContext *dc,
                                        GenericIdentTypeRepr *generic,
                                        TypeResolutionOptions options,
                                        GenericTypeResolver *resolver) {
  assert(!options.contains(TypeResolutionFlags::ResolveStructure) &&
         "should not touch generic arguments when resolving structure");

  if (type->hasError()) {
    generic->setInvalid();
    return type;
  }

  // We must either have an unbound generic type, or a generic type alias.
  if (!type->is<UnboundGenericType>()) {
     if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      auto diag = diagnose(loc, diag::not_a_generic_type, type);

      // Don't add fixit on module type; that isn't the right type regardless
      // of whether it had generic arguments.
      if (!type->is<ModuleType>()) {
        // When turning a SourceRange into CharSourceRange the closing angle
        // brackets on nested generics are lexed as one token.
        SourceRange angles = generic->getAngleBrackets();
        diag.fixItRemoveChars(angles.Start,
                              angles.End.getAdvancedLocOrInvalid(1));
      }

      generic->setInvalid();
    }
    return type;
  }

  auto *unboundType = type->castTo<UnboundGenericType>();
  auto *decl = unboundType->getDecl();

  // Make sure we have the right number of generic arguments.
  // FIXME: If we have fewer arguments than we need, that might be okay, if
  // we're allowed to deduce the remaining arguments from context.
  auto genericDecl = cast<GenericTypeDecl>(decl);
  auto genericArgs = generic->getGenericArgs();
  auto genericParams = genericDecl->getGenericParams();
  if (genericParams->size() != genericArgs.size()) {
    if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      diagnose(loc, diag::type_parameter_count_mismatch, decl->getName(),
               genericParams->size(), genericArgs.size(),
               genericArgs.size() < genericParams->size())
          .highlight(generic->getAngleBrackets());
      diagnose(decl, diag::kind_identifier_declared_here,
               DescriptiveDeclKind::GenericType, decl->getName());
    }
    return ErrorType::get(Context);
  }

  // In SIL mode, Optional<T> interprets T as a SIL type.
  if (options.contains(TypeResolutionFlags::SILType)) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(decl)) {
      if (nominal->isOptionalDecl()) {
        // Validate the generic argument.
        TypeLoc arg = genericArgs[0];
        if (validateType(arg, dc, withoutContext(options, true), resolver))
          return nullptr;

        Type objectType = arg.getType();
        if (!objectType)
          return nullptr;

        return BoundGenericType::get(nominal, /*parent*/ Type(), objectType);
      }
    }  
  }

  // Cannot extend a bound generic type.
  if (options.contains(TypeResolutionFlags::ExtensionBinding)) {
    if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      diagnose(loc, diag::extension_specialization,
               genericDecl->getName())
        .highlight(generic->getSourceRange());
    }
    return ErrorType::get(Context);
  }

  // FIXME: More principled handling of circularity.
  if (!genericDecl->hasValidSignature()) {
    diagnose(loc, diag::recursive_type_reference,
             genericDecl->getDescriptiveKind(), genericDecl->getName());
    diagnose(genericDecl, diag::kind_declared_here,
             DescriptiveDeclKind::Type);
    return ErrorType::get(Context);
  }

  // Resolve the types of the generic arguments.
  assert(!options.contains(TypeResolutionFlags::ResolveStructure) &&
         "should not touch generic arguments when resolving structure");
  options = adjustOptionsForGenericArgs(options);

  SmallVector<Type, 2> args;
  for (auto tyR : genericArgs) {
    // Propagate failure.
    TypeLoc genericArg = tyR;
    if (validateType(genericArg, dc, options, resolver))
      return ErrorType::get(Context);

    auto substTy = genericArg.getType();

    // Unsatisfied dependency case.
    if (!substTy)
      return nullptr;

    args.push_back(substTy);
  }

  auto result = applyUnboundGenericArguments(unboundType, genericDecl, loc,
                                             dc, args, resolver);
  if (!result)
    return result;

  // Migration hack.
  bool isMutablePointer;
  if (isPointerToVoid(dc->getASTContext(), result, isMutablePointer)) {
    if (isMutablePointer)
      diagnose(loc, diag::use_of_void_pointer, "Mutable").
        fixItReplace(generic->getSourceRange(), "UnsafeMutableRawPointer");
    else
      diagnose(loc, diag::use_of_void_pointer, "").
        fixItReplace(generic->getSourceRange(), "UnsafeRawPointer");
  }
  return result;
}

/// Apply generic arguments to the given type.
Type TypeChecker::applyUnboundGenericArguments(
    UnboundGenericType *unboundType, GenericTypeDecl *decl,
    SourceLoc loc, DeclContext *dc,
    ArrayRef<Type> genericArgs,
    GenericTypeResolver *resolver) {
  assert(genericArgs.size() == decl->getGenericParams()->size() &&
         "invalid arguments, use applyGenericArguments for diagnostic emitting");

  // Make sure we always have a resolver to use.
  GenericTypeToArchetypeResolver defaultResolver(dc);
  if (!resolver)
    resolver = &defaultResolver;

  auto genericSig = decl->getGenericSignature();
  assert(genericSig != nullptr);

  TypeSubstitutionMap subs;

  // Get the interface type for the declaration. We will be substituting
  // type parameters that appear inside this type with the provided
  // generic arguments.
  auto resultType = decl->getDeclaredInterfaceType();

  bool hasTypeVariable = false;

  // Get the substitutions for outer generic parameters from the parent
  // type.
  if (auto parentType = unboundType->getParent()) {
    if (parentType->hasUnboundGenericType()) {
      // If we're working with a nominal type declaration, just construct
      // a bound generic type without checking the generic arguments.
      if (auto *nominalDecl = dyn_cast<NominalTypeDecl>(decl)) {
        return BoundGenericType::get(nominalDecl, parentType, genericArgs);
      }

      assert(!resultType->hasTypeParameter());
      return resultType;
    }

    subs = parentType->getContextSubstitutions(decl->getDeclContext());
    hasTypeVariable |= parentType->hasTypeVariable();
  }

  SourceLoc noteLoc = decl->getLoc();
  if (noteLoc.isInvalid())
    noteLoc = loc;

  // Realize the types of the generic arguments and add them to the
  // substitution map.
  for (unsigned i = 0, e = genericArgs.size(); i < e; i++) {
    auto origTy = genericSig->getInnermostGenericParams()[i];
    auto substTy = genericArgs[i];

    // Enter a substitution.
    subs[origTy->getCanonicalType()->castTo<GenericTypeParamType>()] =
      substTy;

    hasTypeVariable |= substTy->hasTypeVariable();
  }

  // Check the generic arguments against the requirements of the declaration's
  // generic signature.
  if (!hasTypeVariable) {
    auto result =
      checkGenericArguments(dc, loc, noteLoc, unboundType,
                            genericSig->getGenericParams(),
                            genericSig->getRequirements(),
                            QueryTypeSubstitutionMap{subs},
                            LookUpConformance(*this, dc));

    switch (result) {
    case RequirementCheckResult::Failure:
    case RequirementCheckResult::SubstitutionFailure:
      return ErrorType::get(Context);
    case RequirementCheckResult::Success:
      break;
    }
  }

  // For a typealias, use the underlying type. We'll wrap up the result
  // later.
  auto typealias = dyn_cast<TypeAliasDecl>(decl);
  if (typealias) {
    resultType = typealias->getUnderlyingTypeLoc().getType();
  }

  // Apply the substitution map to the interface type of the declaration.
  resultType = resultType.subst(QueryTypeSubstitutionMap{subs},
                                LookUpConformance(*this, dc),
                                SubstFlags::UseErrorType);

  // Form a sugared typealias reference.
  Type parentType = unboundType->getParent();
  if (typealias && (!parentType || !parentType->isAnyExistentialType())) {
    auto genericSig = typealias->getGenericSignature();
    auto subMap = SubstitutionMap::get(genericSig,
                                       QueryTypeSubstitutionMap{subs},
                                       LookUpConformance(*this, dc));
    resultType = NameAliasType::get(typealias, parentType,
                                    subMap, resultType);
  }

  if (isa<NominalTypeDecl>(decl) && resultType) {
    (void)useObjectiveCBridgeableConformancesOfArgs(
        dc, resultType->castTo<BoundGenericType>());
  }

  return resultType;
}

/// \brief Diagnose a use of an unbound generic type.
static void diagnoseUnboundGenericType(TypeChecker &tc, Type ty,SourceLoc loc) {
  auto unbound = ty->castTo<UnboundGenericType>();
  {
    InFlightDiagnostic diag = tc.diagnose(loc,
        diag::generic_type_requires_arguments, ty);
    if (auto *genericD = unbound->getDecl()) {
      SmallString<64> genericArgsToAdd;
      if (tc.getDefaultGenericArgumentsString(genericArgsToAdd, genericD))
        diag.fixItInsertAfter(loc, genericArgsToAdd);
    }
  }
  tc.diagnose(unbound->getDecl(), diag::kind_identifier_declared_here,
              DescriptiveDeclKind::GenericType, unbound->getDecl()->getName());
}

// Produce a diagnostic if the type we referenced was an
// associated type but the type itself was erroneous. We'll produce a
// diagnostic here if the diagnostic for the bad type witness would show up in
// a different context.
static void maybeDiagnoseBadConformanceRef(TypeChecker &tc,
                                           DeclContext *dc,
                                           Type parentTy,
                                           SourceLoc loc,
                                           AssociatedTypeDecl *assocType) {
  // If we weren't given a conformance, go look it up.
  ProtocolConformance *conformance = nullptr;
  if (auto conformanceRef = tc.conformsToProtocol(
          parentTy, assocType->getProtocol(), dc,
          (ConformanceCheckFlags::InExpression |
           ConformanceCheckFlags::SuppressDependencyTracking |
           ConformanceCheckFlags::AllowUnavailableConditionalRequirements))) {
    if (conformanceRef->isConcrete())
      conformance = conformanceRef->getConcrete();
  }

  // If any errors have occurred, don't bother diagnosing this cross-file
  // issue.
  if (tc.Context.Diags.hadAnyError())
    return;

  auto diagCode =
      (conformance && !conformance->getConditionalRequirementsIfAvailable())
          ? diag::unsupported_recursion_in_associated_type_reference
          : diag::broken_associated_type_witness;

  tc.diagnose(loc, diagCode, assocType->getFullName(), parentTy);
}

/// \brief Returns a valid type or ErrorType in case of an error.
static Type resolveTypeDecl(TypeChecker &TC, TypeDecl *typeDecl, SourceLoc loc,
                            DeclContext *foundDC,
                            DeclContext *fromDC,
                            GenericIdentTypeRepr *generic,
                            TypeResolutionOptions options,
                            GenericTypeResolver *resolver) {
  assert(fromDC && "No declaration context for type resolution?");

  // Don't validate nominal type declarations during extension binding.
  if (!options.contains(TypeResolutionFlags::ExtensionBinding) ||
      !isa<NominalTypeDecl>(typeDecl)) {
    // Validate the declaration.
    TC.validateDeclForNameLookup(typeDecl);

    // If we were not able to validate recursively, bail out.
    if (!typeDecl->hasInterfaceType()) {
      TC.diagnose(loc, diag::recursive_type_reference,
                  typeDecl->getDescriptiveKind(), typeDecl->getName());
      TC.diagnose(typeDecl->getLoc(), diag::kind_declared_here,
                  DescriptiveDeclKind::Type);
      return ErrorType::get(TC.Context);
    }
  }

  // Resolve the type declaration to a specific type. How this occurs
  // depends on the current context and where the type was found.
  Type type =
    TC.resolveTypeInContext(typeDecl, foundDC, fromDC, options,
                            generic, resolver);

  if (type->is<UnboundGenericType>() && !generic &&
      !options.contains(TypeResolutionFlags::AllowUnboundGenerics) &&
      !options.contains(TypeResolutionFlags::TypeAliasUnderlyingType) &&
      !options.contains(TypeResolutionFlags::ResolveStructure)) {
    diagnoseUnboundGenericType(TC, type, loc);
    return ErrorType::get(TC.Context);
  }

  if (type->hasError() && isa<AssociatedTypeDecl>(typeDecl)) {
    maybeDiagnoseBadConformanceRef(TC, fromDC,
                                   foundDC->getDeclaredInterfaceType(),
                                   loc, cast<AssociatedTypeDecl>(typeDecl));
  }

  if (generic && !options.contains(TypeResolutionFlags::ResolveStructure)) {
    // Apply the generic arguments to the type.
    type = TC.applyGenericArguments(type, loc, fromDC, generic,
                                    options, resolver);
    if (!type)
      return nullptr;
  }

  assert(type);
  return type;
}

static std::string getDeclNameFromContext(DeclContext *dc,
                                          NominalTypeDecl *nominal) {
  // We don't allow an unqualified reference to a type inside an
  // extension if the type is itself nested inside another type,
  // eg:
  //
  // extension A.B { ... B ... }
  //
  // Instead, you must write 'A.B'. Calculate the right name to use
  // for fixits.
  if (!isa<ExtensionDecl>(dc)) {
    SmallVector<Identifier, 2> idents;
    auto *parentNominal = nominal;
    while (parentNominal != nullptr) {
      idents.push_back(parentNominal->getName());
      parentNominal = parentNominal->getDeclContext()
        ->getAsNominalTypeOrNominalTypeExtensionContext();
    }
    std::reverse(idents.begin(), idents.end());
    std::string result;
    for (auto ident : idents) {
      if (!result.empty())
        result += ".";
      result += ident.str();
    }
    return result;
  } else {
    return nominal->getName().str();
  }
}

/// Diagnose a reference to an unknown type.
///
/// This routine diagnoses a reference to an unknown type, and
/// attempts to fix the reference via various means.
///
/// \param tc The type checker through which we should emit the diagnostic.
/// \param dc The context in which name lookup occurred.
///
/// \returns either the corrected type, if possible, or an error type to
/// that correction failed.
static Type diagnoseUnknownType(TypeChecker &tc, DeclContext *dc,
                                Type parentType,
                                SourceRange parentRange,
                                ComponentIdentTypeRepr *comp,
                                TypeResolutionOptions options,
                                NameLookupOptions lookupOptions,
                                GenericTypeResolver *resolver) {
  // Unqualified lookup case.
  if (parentType.isNull()) {
    if (comp->getIdentifier() == tc.Context.Id_Self &&
        !isa<GenericIdentTypeRepr>(comp)) {
      DeclContext *nominalDC = nullptr;
      NominalTypeDecl *nominal = nullptr;
      if ((nominalDC = dc->getInnermostTypeContext()) &&
          (nominal = nominalDC->getAsNominalTypeOrNominalTypeExtensionContext())) {
        // Attempt to refer to 'Self' within a non-protocol nominal
        // type. Fix this by replacing 'Self' with the nominal type name.
        assert(!isa<ProtocolDecl>(nominal) && "Cannot be a protocol");

        // Produce a Fix-It replacing 'Self' with the nominal type name.
        auto name = getDeclNameFromContext(dc, nominal);
        tc.diagnose(comp->getIdLoc(), diag::self_in_nominal, name)
          .fixItReplace(comp->getIdLoc(), name);

        // If this is a requirement, replacing 'Self' with a valid type will
        // result in additional unnecessary diagnostics (does not refer to a
        // generic parameter or associated type). Simply return an error type.
        if (options.contains(TypeResolutionFlags::GenericRequirement))
          return ErrorType::get(tc.Context);

        auto type = resolver->mapTypeIntoContext(
          dc->getInnermostTypeContext()->getSelfInterfaceType());

        comp->overwriteIdentifier(nominal->getName());
        comp->setValue(nominal, nominalDC->getParent());
        return type;
      }
      // Attempt to refer to 'Self' from a free function.
      tc.diagnose(comp->getIdLoc(), diag::dynamic_self_non_method,
                  dc->getParent()->isLocalContext());

      return ErrorType::get(tc.Context);
    }

    // Try ignoring access control.
    DeclContext *lookupDC = dc;
    if (options.contains(TypeResolutionFlags::GenericSignature))
      lookupDC = dc->getParentForLookup();

    NameLookupOptions relookupOptions = lookupOptions;
    relookupOptions |= NameLookupFlags::KnownPrivate;
    relookupOptions |= NameLookupFlags::IgnoreAccessControl;
    auto inaccessibleResults =
        tc.lookupUnqualifiedType(lookupDC, comp->getIdentifier(), comp->getIdLoc(),
                                 relookupOptions);
    if (!inaccessibleResults.empty()) {
      // FIXME: What if the unviable candidates have different levels of access?
      auto first = cast<TypeDecl>(inaccessibleResults.front().getValueDecl());
      tc.diagnose(comp->getIdLoc(), diag::candidate_inaccessible,
                  comp->getIdentifier(), first->getFormalAccess());

      // FIXME: If any of the candidates (usually just one) are in the same
      // module we could offer a fix-it.
      for (auto lookupResult : inaccessibleResults)
        tc.diagnose(lookupResult.getValueDecl(), diag::kind_declared_here,
                    DescriptiveDeclKind::Type);

      // Don't try to recover here; we'll get more access-related diagnostics
      // downstream if we do.
      return ErrorType::get(tc.Context);
    }

    // Fallback.
    SourceLoc L = comp->getIdLoc();
    SourceRange R = SourceRange(comp->getIdLoc());

    // Check if the unknown type is in the type remappings.
    auto &Remapped = tc.Context.RemappedTypes;
    auto TypeName = comp->getIdentifier().str();
    auto I = Remapped.find(TypeName);
    if (I != Remapped.end()) {
      auto RemappedTy = I->second->getString();
      tc.diagnose(L, diag::use_undeclared_type_did_you_mean,
                  comp->getIdentifier(), RemappedTy)
        .highlight(R)
        .fixItReplace(R, RemappedTy);

      // Replace the computed type with the suggested type.
      comp->overwriteIdentifier(tc.Context.getIdentifier(RemappedTy));

      // HACK: 'NSUInteger' suggests both 'UInt' and 'Int'.
      if (TypeName
            == tc.Context.getSwiftName(KnownFoundationEntity::NSUInteger)) {
        tc.diagnose(L, diag::note_remapped_type, "UInt")
          .fixItReplace(R, "UInt");
      }

      return I->second;
    }

    tc.diagnose(L, diag::use_undeclared_type,
                comp->getIdentifier())
      .highlight(R);

    return ErrorType::get(tc.Context);
  }

  // Qualified lookup case.
  if (!parentType->mayHaveMembers()) {
    tc.diagnose(comp->getIdLoc(), diag::invalid_member_type,
                comp->getIdentifier(), parentType)
        .highlight(parentRange);
    return ErrorType::get(tc.Context);
  }

  // Try ignoring access control.
  NameLookupOptions relookupOptions = lookupOptions;
  relookupOptions |= NameLookupFlags::KnownPrivate;
  relookupOptions |= NameLookupFlags::IgnoreAccessControl;
  auto inaccessibleMembers = tc.lookupMemberType(dc, parentType,
                                                 comp->getIdentifier(),
                                                 relookupOptions);
  if (inaccessibleMembers) {
    // FIXME: What if the unviable candidates have different levels of access?
    const TypeDecl *first = inaccessibleMembers.front().Member;
    tc.diagnose(comp->getIdLoc(), diag::candidate_inaccessible,
                comp->getIdentifier(), first->getFormalAccess());

    // FIXME: If any of the candidates (usually just one) are in the same module
    // we could offer a fix-it.
    for (auto lookupResult : inaccessibleMembers)
      tc.diagnose(lookupResult.Member, diag::kind_declared_here,
                  DescriptiveDeclKind::Type);

    // Don't try to recover here; we'll get more access-related diagnostics
    // downstream if we do.
    return ErrorType::get(tc.Context);
  }

  // FIXME: Typo correction!

  // Lookup into a type.
  if (auto moduleType = parentType->getAs<ModuleType>()) {
    tc.diagnose(comp->getIdLoc(), diag::no_module_type,
                comp->getIdentifier(), moduleType->getModule()->getName());
  } else {
    LookupResult memberLookup;
    // Let's try to lookup given identifier as a member of the parent type,
    // this allows for more precise diagnostic, which distinguishes between
    // identifier not found as a member type vs. not found at all.
    NameLookupOptions memberLookupOptions = lookupOptions;
    memberLookupOptions |= NameLookupFlags::IgnoreAccessControl;
    memberLookupOptions |= NameLookupFlags::KnownPrivate;

    memberLookup = tc.lookupMember(dc, parentType, comp->getIdentifier(),
                                   memberLookupOptions);

    // Looks like this is not a member type, but simply a member of parent type.
    if (!memberLookup.empty()) {
      auto member = memberLookup[0].getValueDecl();
      tc.diagnose(comp->getIdLoc(), diag::invalid_member_reference,
                  member->getDescriptiveKind(), comp->getIdentifier(),
                  parentType)
          .highlight(parentRange);
    } else {
      tc.diagnose(comp->getIdLoc(), diag::invalid_member_type,
                  comp->getIdentifier(), parentType)
          .highlight(parentRange);
    }
  }
  return ErrorType::get(tc.Context);
}

static Type
resolveTopLevelIdentTypeComponent(TypeChecker &TC, DeclContext *DC,
                                  ComponentIdentTypeRepr *comp,
                                  TypeResolutionOptions options,
                                  GenericTypeResolver *resolver);

static Type
resolveGenericSignatureComponent(TypeChecker &TC, DeclContext *DC,
                                 ComponentIdentTypeRepr *comp,
                                 TypeResolutionOptions options,
                                 GenericTypeResolver *resolver) {
  if (!DC->isInnermostContextGeneric())
    return Type();

  auto *genericParams = DC->getGenericParamsOfContext();

  if (!isa<ExtensionDecl>(DC)) {
    auto matchingParam =
      std::find_if(genericParams->begin(), genericParams->end(),
                   [comp](const GenericTypeParamDecl *param) {
                     return param->getFullName().matchesRef(comp->getIdentifier());
                   });

    if (matchingParam == genericParams->end())
      return Type();

    comp->setValue(*matchingParam, nullptr);
    return resolveTopLevelIdentTypeComponent(TC, DC, comp, options, resolver);
  }

  // If we are inside an extension of a nested type, we have to visit
  // all outer parameter lists. Otherwise, we will visit them when
  // name lookup goes ahead and checks the outer DeclContext.
  for (auto *outerParams = genericParams;
       outerParams != nullptr;
       outerParams = outerParams->getOuterParameters()) {
    auto matchingParam =
      std::find_if(outerParams->begin(), outerParams->end(),
                   [comp](const GenericTypeParamDecl *param) {
                     return param->getFullName().matchesRef(comp->getIdentifier());
                   });

    if (matchingParam != outerParams->end()) {
      comp->setValue(*matchingParam, nullptr);
      return resolveTopLevelIdentTypeComponent(TC, DC, comp, options, resolver);
    }
  }

  // If the lookup occurs from within a trailing 'where' clause of
  // a constrained extension, also look for associated types and typealiases
  // in the protocol.
  if (genericParams->hasTrailingWhereClause() &&
      comp->getIdLoc().isValid() &&
      TC.Context.SourceMgr.rangeContainsTokenLoc(
        genericParams->getTrailingWhereClauseSourceRange(),
          comp->getIdLoc())) {
    auto nominal = DC->getAsNominalTypeOrNominalTypeExtensionContext();
    SmallVector<ValueDecl *, 4> decls;
    if (DC->lookupQualified(nominal,
                            comp->getIdentifier(),
                            NL_OnlyTypes|NL_QualifiedDefault|NL_ProtocolMembers,
                            decls)) {
      for (const auto decl : decls) {
        // FIXME: Better ambiguity handling.
        auto typeDecl = cast<TypeDecl>(decl);

        if (!isa<ProtocolDecl>(typeDecl->getDeclContext())) continue;

        comp->setValue(typeDecl, DC);
        return resolveTopLevelIdentTypeComponent(TC, DC, comp, options,
                                                 resolver);
      }
    }
  }

  return Type();
}

/// Resolve the given identifier type representation as an unqualified type,
/// returning the type it references.
///
/// \returns Either the resolved type or a null type, the latter of
/// which indicates that some dependencies were unsatisfied.
static Type
resolveTopLevelIdentTypeComponent(TypeChecker &TC, DeclContext *DC,
                                  ComponentIdentTypeRepr *comp,
                                  TypeResolutionOptions options,
                                  GenericTypeResolver *resolver) {
  // Short-circuiting.
  if (comp->isInvalid()) return ErrorType::get(TC.Context);

  // If the component has already been bound to a declaration, handle
  // that now.
  if (auto *typeDecl = comp->getBoundDecl()) {
    // Resolve the type declaration within this context.
    return resolveTypeDecl(TC, typeDecl, comp->getIdLoc(),
                           comp->getDeclContext(), DC,
                           dyn_cast<GenericIdentTypeRepr>(comp), options,
                           resolver);
  }

  // Resolve the first component, which is the only one that requires
  // unqualified name lookup.
  DeclContext *lookupDC = DC;

  // Dynamic 'Self' in the result type of a function body.
  if (options.contains(TypeResolutionFlags::DynamicSelfResult) &&
      comp->getIdentifier() == TC.Context.Id_Self) {
    auto func = cast<FuncDecl>(DC);
    assert(func->hasDynamicSelf() && "Not marked as having dynamic Self?");

    // FIXME: The passed-in TypeRepr should get 'typechecked' as well.
    // The issue is though that ComponentIdentTypeRepr only accepts a ValueDecl
    // while the 'Self' type is more than just a reference to a TypeDecl.

    auto selfType = resolver->mapTypeIntoContext(
      func->getDeclContext()->getSelfInterfaceType());
    return DynamicSelfType::get(selfType, TC.Context);
  }

  // For lookups within the generic signature, look at the generic
  // parameters (only), then move up to the enclosing context.
  if (options.contains(TypeResolutionFlags::GenericSignature)) {
    Type type = resolveGenericSignatureComponent(
      TC, DC, comp, options, resolver);
    if (type)
      return type;

    if (!DC->isCascadingContextForLookup(/*excludeFunctions*/false))
      options |= TypeResolutionFlags::KnownNonCascadingDependency;

    lookupDC = DC->getParentForLookup();
  }

  auto id = comp->getIdentifier();

  NameLookupOptions lookupOptions = defaultUnqualifiedLookupOptions;
  if (options.contains(TypeResolutionFlags::KnownNonCascadingDependency))
    lookupOptions |= NameLookupFlags::KnownPrivate;
  auto globals = TC.lookupUnqualifiedType(lookupDC,
                                          id,
                                          comp->getIdLoc(),
                                          lookupOptions);

  // Process the names we found.
  Type current;
  TypeDecl *currentDecl = nullptr;
  DeclContext *currentDC = nullptr;
  bool isAmbiguous = false;
  for (const auto entry : globals) {
    auto *foundDC = entry.getDeclContext();
    auto *typeDecl = cast<TypeDecl>(entry.getValueDecl());

    Type type = resolveTypeDecl(TC, typeDecl, comp->getIdLoc(),
                                foundDC, DC,
                                dyn_cast<GenericIdentTypeRepr>(comp), options,
                                resolver);

    if (!type)
      return type;

    if (type->is<ErrorType>())
      return type;

    // If this is the first result we found, record it.
    if (current.isNull()) {
      current = type;
      currentDecl = typeDecl;
      currentDC = foundDC;
      continue;
    }

    // Otherwise, check for an ambiguity.
    if (!resolver->areSameType(current, type)) {
      isAmbiguous = true;
      break;
    }

    // We have a found multiple type aliases that refer to the same thing.
    // Ignore the duplicate.
  }

  // Complain about any ambiguities we detected.
  // FIXME: We could recover by looking at later components.
  if (isAmbiguous) {
    if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      TC.diagnose(comp->getIdLoc(), diag::ambiguous_type_base,
                  comp->getIdentifier())
        .highlight(comp->getIdLoc());
      for (auto entry : globals) {
        TC.diagnose(entry.getValueDecl(), diag::found_candidate);
      }
    }

    comp->setInvalid();
    return ErrorType::get(TC.Context);
  }

  // If we found nothing, complain and give ourselves a chance to recover.
  if (current.isNull()) {
    // If we're not allowed to complain or we couldn't fix the
    // source, bail out.
    if (options.contains(TypeResolutionFlags::SilenceErrors))
      return ErrorType::get(TC.Context);

    return diagnoseUnknownType(TC, DC, nullptr, SourceRange(), comp, options,
                               lookupOptions, resolver);
  }

  comp->setValue(currentDecl, currentDC);
  return current;
}

/// Resolve the given identifier type representation as a qualified
/// lookup within the given parent type, returning the type it
/// references.
static Type resolveNestedIdentTypeComponent(
              TypeChecker &TC, DeclContext *DC,
              Type parentTy,
              SourceRange parentRange,
              ComponentIdentTypeRepr *comp,
              TypeResolutionOptions options,
              GenericTypeResolver *resolver) {
  auto maybeApplyGenericArgs = [&](Type memberType) {
    // If there are generic arguments, apply them now.
    if (!options.contains(TypeResolutionFlags::ResolveStructure)) {
      if (auto genComp = dyn_cast<GenericIdentTypeRepr>(comp)) {
        return TC.applyGenericArguments(memberType, comp->getIdLoc(),
                                        DC, genComp, options, resolver);
      }
    }

    if (memberType->is<UnboundGenericType>() &&
        !options.contains(TypeResolutionFlags::AllowUnboundGenerics) &&
        !options.contains(TypeResolutionFlags::TypeAliasUnderlyingType) &&
        !options.contains(TypeResolutionFlags::ResolveStructure)) {
      diagnoseUnboundGenericType(TC, memberType, comp->getLoc());
      return ErrorType::get(TC.Context);
    }

    return memberType;
  };

  auto maybeDiagnoseBadMemberType = [&](TypeDecl *member, Type memberType,
                                        AssociatedTypeDecl *inferredAssocType) {
    // Diagnose invalid cases.
    if (TC.isUnsupportedMemberTypeAccess(parentTy, member)) {
      if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
        if (parentTy->is<UnboundGenericType>())
          diagnoseUnboundGenericType(TC, parentTy, parentRange.End);
        else if (parentTy->isExistentialType() &&
                 isa<AssociatedTypeDecl>(member)) {
          TC.diagnose(comp->getIdLoc(), diag::assoc_type_outside_of_protocol,
                      comp->getIdentifier());
        } else if (parentTy->isExistentialType() &&
                   isa<TypeAliasDecl>(member)) {
          TC.diagnose(comp->getIdLoc(), diag::typealias_outside_of_protocol,
                      comp->getIdentifier());
        }
      }

      return ErrorType::get(TC.Context);
    }

    // Only the last component of the underlying type of a type alias may
    // be an unbound generic.
    if (options & TypeResolutionFlags::TypeAliasUnderlyingType) {
      if (parentTy->is<UnboundGenericType>()) {
        if (!options.contains(TypeResolutionFlags::SilenceErrors))
          diagnoseUnboundGenericType(TC, parentTy, parentRange.End);

        return ErrorType::get(TC.Context);
      }
    }

    // Diagnose a bad conformance reference if we need to.
    if (!options.contains(TypeResolutionFlags::SilenceErrors) &&
        inferredAssocType && memberType && memberType->hasError()) {
      maybeDiagnoseBadConformanceRef(TC, DC, parentTy, comp->getLoc(),
                                     inferredAssocType);
    }

    // If we found a reference to an associated type or other member type that
    // was marked invalid, just return ErrorType to silence downstream errors.
    if (member->isInvalid())
      return ErrorType::get(TC.Context);

    // At this point, we need to have resolved the type of the member.
    if (!memberType || memberType->hasError())
      return memberType;

    // If there are generic arguments, apply them now.
    return maybeApplyGenericArgs(memberType);
  };

  // Short-circuiting.
  if (comp->isInvalid()) return ErrorType::get(TC.Context);

  // If the parent is a type parameter, the member is a dependent member,
  // and we skip much of the work below.
  if (parentTy->isTypeParameter()) {
    if (auto memberType = resolver->resolveDependentMemberType(
            parentTy, DC, parentRange, comp)) {
      // Hack -- if we haven't resolved this to a declaration yet, don't
      // attempt to apply generic arguments, since this will emit a
      // diagnostic, and its possible that this type will become a concrete
      // type later on.
      if (!memberType->is<DependentMemberType>() ||
          memberType->castTo<DependentMemberType>()->getAssocType()) {
        return maybeApplyGenericArgs(memberType);
      }

      return memberType;
    }
  }

  // Phase 2: If a declaration has already been bound, use it.
  if (auto *typeDecl = comp->getBoundDecl()) {
    auto memberType = TC.substMemberTypeWithBase(DC->getParentModule(),
                                                 typeDecl, parentTy);
    return maybeDiagnoseBadMemberType(typeDecl, memberType, nullptr);
  }

  // Phase 1: Find and bind the component decl.

  // Look for member types with the given name.
  bool isKnownNonCascading = options.contains(TypeResolutionFlags::KnownNonCascadingDependency);
  if (!isKnownNonCascading && options.contains(TypeResolutionFlags::InExpression)) {
    // Expressions cannot affect a function's signature.
    isKnownNonCascading = isa<AbstractFunctionDecl>(DC);
  }

  NameLookupOptions lookupOptions = defaultMemberLookupOptions;
  if (isKnownNonCascading)
    lookupOptions |= NameLookupFlags::KnownPrivate;
  // FIXME: Lift the restriction for TypeResolutionFlags::InheritanceClause
  if (options.contains(TypeResolutionFlags::ExtensionBinding) ||
      options.contains(TypeResolutionFlags::InheritanceClause))
    lookupOptions -= NameLookupFlags::ProtocolMembers;
  LookupTypeResult memberTypes;
  if (parentTy->mayHaveMembers())
    memberTypes = TC.lookupMemberType(DC, parentTy, comp->getIdentifier(),
                                      lookupOptions);

  // Name lookup was ambiguous. Complain.
  // FIXME: Could try to apply generic arguments first, and see whether
  // that resolves things. But do we really want that to succeed?
  if (memberTypes.size() > 1) {
    if (!options.contains(TypeResolutionFlags::SilenceErrors))
      TC.diagnoseAmbiguousMemberType(parentTy, parentRange,
                                     comp->getIdentifier(), comp->getIdLoc(),
                                     memberTypes);
    return ErrorType::get(TC.Context);
  }

  // If we didn't find anything, complain.
  Type memberType;
  TypeDecl *member = nullptr;
  AssociatedTypeDecl *inferredAssocType = nullptr;
  if (!memberTypes) {
    // If we're not allowed to complain or we couldn't fix the
    // source, bail out.
    if (options.contains(TypeResolutionFlags::SilenceErrors))
      return ErrorType::get(TC.Context);

    memberType = diagnoseUnknownType(TC, DC, parentTy, parentRange, comp,
                                     options, lookupOptions, resolver);
    member = comp->getBoundDecl();
    if (!member)
      return ErrorType::get(TC.Context);
  } else {
    memberType = memberTypes.back().MemberType;
    member = memberTypes.back().Member;
    inferredAssocType = memberTypes.back().InferredAssociatedType;
    comp->setValue(member, nullptr);
  }

  return maybeDiagnoseBadMemberType(member, memberType, inferredAssocType);
}

static Type resolveIdentTypeComponent(
              TypeChecker &TC, DeclContext *DC,
              ArrayRef<ComponentIdentTypeRepr *> components,
              TypeResolutionOptions options,
              GenericTypeResolver *resolver) {
  auto comp = components.back();

  // The first component uses unqualified lookup.
  auto parentComps = components.slice(0, components.size()-1);
  if (parentComps.empty()) {
    return resolveTopLevelIdentTypeComponent(TC, DC, comp, options, resolver);
  }

  // All remaining components use qualified lookup.

  // Resolve the parent type.
  Type parentTy = resolveIdentTypeComponent(TC, DC, parentComps, options,
                                            resolver);
  if (!parentTy || parentTy->hasError()) return parentTy;
  
  SourceRange parentRange(parentComps.front()->getIdLoc(),
                          parentComps.back()->getSourceRange().End);

  // Resolve the nested type.
  return resolveNestedIdentTypeComponent(TC, DC, parentTy,
                                         parentRange, comp,
                                         options, resolver);
}

static bool diagnoseAvailability(IdentTypeRepr *IdType,
                                 DeclContext *DC, TypeChecker &TC,
                                 bool AllowPotentiallyUnavailableProtocol) {
  auto componentRange = IdType->getComponentRange();
  for (auto comp : componentRange) {
    if (auto *typeDecl = comp->getBoundDecl()) {
      // In Swift 3, components other than the last one were not properly
      // checked for availability.
      // FIXME: We should try to downgrade these errors to warnings, not just
      // skip diagnosing them.
      if (TC.getLangOpts().isSwiftVersion3() && comp != componentRange.back())
        continue;

      if (diagnoseDeclAvailability(typeDecl, TC, DC, comp->getIdLoc(),
                                   AllowPotentiallyUnavailableProtocol,
                                   /*SignalOnPotentialUnavailability*/false)) {
        return true;
      }
    }
  }

  return false;
}

// Hack to apply context-specific @escaping to an AST function type.
static Type applyNonEscapingFromContext(DeclContext *DC,
                                        Type ty,
                                        TypeResolutionOptions options) {
  // Remember whether this is a function parameter.
  bool defaultNoEscape =
    !options.contains(TypeResolutionFlags::EnumCase) &&
    options.contains(TypeResolutionFlags::FunctionInput);

  // Desugar here
  auto *funcTy = ty->castTo<FunctionType>();
  auto extInfo = funcTy->getExtInfo();
  if (defaultNoEscape && !extInfo.isNoEscape()) {
    extInfo = extInfo.withNoEscape();

    // We lost the sugar to flip the isNoEscape bit.
    //
    // FIXME: It would be better to add a new AttributedType sugared type,
    // which would wrap the NameAliasType or ParenType, and apply the
    // isNoEscape bit when de-sugaring.
    // <https://bugs.swift.org/browse/SR-2520>
    return FunctionType::get(funcTy->getParams(), funcTy->getResult(), extInfo);
  }

  // Note: original sugared type
  return ty;
}

/// \brief Returns a valid type or ErrorType in case of an error.
Type TypeChecker::resolveIdentifierType(
       DeclContext *DC,
       IdentTypeRepr *IdType,
       TypeResolutionOptions options,
       GenericTypeResolver *resolver) {
  assert(resolver && "Missing generic type resolver");

  auto ComponentRange = IdType->getComponentRange();
  auto Components = llvm::makeArrayRef(ComponentRange.begin(),
                                       ComponentRange.end());
  Type result = resolveIdentTypeComponent(*this, DC, Components, options, 
                                          resolver);
  if (!result) return nullptr;

  if (auto moduleTy = result->getAs<ModuleType>()) {
    if (!options.contains(TypeResolutionFlags::SilenceErrors)) {
      auto moduleName = moduleTy->getModule()->getName();
      diagnose(Components.back()->getIdLoc(),
               diag::use_undeclared_type, moduleName);
      diagnose(Components.back()->getIdLoc(),
               diag::note_module_as_type, moduleName);
    }
    Components.back()->setInvalid();
    return ErrorType::get(Context);
  }

  // Hack to apply context-specific @escaping to a typealias with an underlying
  // function type.
  if (result->is<FunctionType>())
    result = applyNonEscapingFromContext(DC, result, options);

  // Check the availability of the type.

  // We allow a type to conform to a protocol that is less available than
  // the type itself. This enables a type to retroactively model or directly
  // conform to a protocol only available on newer OSes and yet still be used on
  // older OSes.
  // To support this, inside inheritance clauses we allow references to
  // protocols that are unavailable in the current type refinement context.

  if (!options.contains(TypeResolutionFlags::SilenceErrors) &&
      !options.contains(TypeResolutionFlags::AllowUnavailable) &&
      diagnoseAvailability(IdType, DC, *this,
             options.contains(TypeResolutionFlags::AllowUnavailableProtocol))) {
    Components.back()->setInvalid();
    return ErrorType::get(Context);
  }
  
  return result;
}

bool TypeChecker::validateType(TypeLoc &Loc, DeclContext *DC,
                               TypeResolutionOptions options,
                               GenericTypeResolver *resolver) {
  // FIXME: Verify that these aren't circular and infinite size.
  assert(!options.contains(TypeResolutionFlags::ResolveStructure) &&
         "ResolveStructure does not do full validation");

  // If we've already validated this type, don't do so again.
  if (Loc.wasValidated())
    return Loc.isError();

  if (Context.Stats)
    Context.Stats->getFrontendCounters().NumTypesValidated++;

  Type type = Loc.getType();
  if (type.isNull()) {
    type = resolveType(Loc.getTypeRepr(), DC, options, resolver);
    if (!type) {
      type = ErrorType::get(Context);

      // Diagnose types that are illegal in SIL.
    } else if (options.contains(TypeResolutionFlags::SILType)
               && !type->isLegalSILType()) {
      diagnose(Loc.getLoc(), diag::illegal_sil_type, type);
      Loc.setInvalidType(Context);
      return true;
    }
  }

  Loc.setType(type);
  return type->hasError();
}

namespace {
  const auto DefaultParameterConvention = ParameterConvention::Direct_Unowned;
  const auto DefaultResultConvention = ResultConvention::Unowned;

  class TypeResolver {
    TypeChecker &TC;
    ASTContext &Context;
    DeclContext *DC;
    GenericTypeResolver *Resolver;

  public:
    TypeResolver(TypeChecker &tc, DeclContext *DC,
                 GenericTypeResolver *resolver)
      : TC(tc), Context(tc.Context), DC(DC), Resolver(resolver)
    {
      assert(resolver);
    }

    Type resolveType(TypeRepr *repr, TypeResolutionOptions options);

  private:

    Type resolveAttributedType(AttributedTypeRepr *repr,
                               TypeResolutionOptions options);
    Type resolveAttributedType(TypeAttributes &attrs, TypeRepr *repr,
                               TypeResolutionOptions options);
    Type resolveASTFunctionType(FunctionTypeRepr *repr,
                                TypeResolutionOptions options,
                                FunctionType::ExtInfo extInfo
                                  = FunctionType::ExtInfo());
    bool
    resolveASTFunctionTypeParams(TupleTypeRepr *inputRepr,
                                 TypeResolutionOptions options,
                                 bool requiresMappingOut,
                                 SmallVectorImpl<AnyFunctionType::Param> &ps);

    Type resolveSILFunctionType(FunctionTypeRepr *repr,
                                TypeResolutionOptions options,
                                SILCoroutineKind coroutineKind
                                  = SILCoroutineKind::None,
                                SILFunctionType::ExtInfo extInfo
                                  = SILFunctionType::ExtInfo(),
                                ParameterConvention calleeConvention
                                  = DefaultParameterConvention,
                                TypeRepr *witnessmethodProtocol = nullptr);
    SILParameterInfo resolveSILParameter(TypeRepr *repr,
                                         TypeResolutionOptions options);
    SILYieldInfo resolveSILYield(TypeAttributes &remainingAttrs,
                                 TypeRepr *repr, TypeResolutionOptions options);
    bool resolveSILResults(TypeRepr *repr, TypeResolutionOptions options,
                           SmallVectorImpl<SILYieldInfo> &yields,
                           SmallVectorImpl<SILResultInfo> &results,
                           Optional<SILResultInfo> &errorResult);
    bool resolveSingleSILResult(TypeRepr *repr, TypeResolutionOptions options,
                                SmallVectorImpl<SILYieldInfo> &yields,
                                SmallVectorImpl<SILResultInfo> &results,
                                Optional<SILResultInfo> &errorResult);
    Type resolveSpecifierTypeRepr(SpecifierTypeRepr *repr,
                                  TypeResolutionOptions options);
    Type resolveArrayType(ArrayTypeRepr *repr,
                          TypeResolutionOptions options);
    Type resolveDictionaryType(DictionaryTypeRepr *repr,
                               TypeResolutionOptions options);
    Type resolveOptionalType(OptionalTypeRepr *repr,
                             TypeResolutionOptions options);
    Type resolveImplicitlyUnwrappedOptionalType(ImplicitlyUnwrappedOptionalTypeRepr *repr,
                                                TypeResolutionOptions options,
                                                bool isDirect);
    Type resolveTupleType(TupleTypeRepr *repr,
                          TypeResolutionOptions options);
    Type resolveCompositionType(CompositionTypeRepr *repr,
                                TypeResolutionOptions options);
    Type resolveMetatypeType(MetatypeTypeRepr *repr,
                             TypeResolutionOptions options);
    Type resolveProtocolType(ProtocolTypeRepr *repr,
                             TypeResolutionOptions options);
    Type resolveSILBoxType(SILBoxTypeRepr *repr,
                           TypeResolutionOptions options);

    Type buildMetatypeType(MetatypeTypeRepr *repr,
                           Type instanceType,
                           Optional<MetatypeRepresentation> storedRepr);
    Type buildProtocolType(ProtocolTypeRepr *repr,
                           Type instanceType,
                           Optional<MetatypeRepresentation> storedRepr);
  };
} // end anonymous namespace

Type TypeChecker::resolveType(TypeRepr *TyR, DeclContext *DC,
                              TypeResolutionOptions options,
                              GenericTypeResolver *resolver) {
  PrettyStackTraceTypeRepr stackTrace(Context, "resolving", TyR);

  // Make sure we always have a resolver to use.
  GenericTypeToArchetypeResolver defaultResolver(DC);
  if (!resolver)
    resolver = &defaultResolver;

  TypeResolver typeResolver(*this, DC, resolver);
  auto result = typeResolver.resolveType(TyR, options);
  
  // If we resolved down to an error, make sure to mark the typeRepr as invalid
  // so we don't produce a redundant diagnostic.
  if (result && result->hasError())
    TyR->setInvalid();
  return result;
}

Type TypeResolver::resolveType(TypeRepr *repr, TypeResolutionOptions options) {
  assert(repr && "Cannot validate null TypeReprs!");

  // If we know the type representation is invalid, just return an
  // error type.
  if (repr->isInvalid()) return ErrorType::get(TC.Context);

  // Strip the "is function input" bits unless this is a type that knows about
  // them.
  if (!isa<SpecifierTypeRepr>(repr) && !isa<TupleTypeRepr>(repr) &&
      !isa<AttributedTypeRepr>(repr) && !isa<FunctionTypeRepr>(repr) &&
      !isa<IdentTypeRepr>(repr) &&
      !isa<ImplicitlyUnwrappedOptionalTypeRepr>(repr)) {
    options -= TypeResolutionFlags::FunctionInput;
    options -= TypeResolutionFlags::TypeAliasUnderlyingType;
  }

  if (Context.LangOpts.DisableAvailabilityChecking)
    options |= TypeResolutionFlags::AllowUnavailable;

  bool isDirect = false;
  if ((options & TypeResolutionFlags::Direct) && !isa<SpecifierTypeRepr>(repr)){
    isDirect = true;
    options -= TypeResolutionFlags::Direct;
  }

  switch (repr->getKind()) {
  case TypeReprKind::Error:
    return ErrorType::get(Context);

  case TypeReprKind::Attributed:
    return resolveAttributedType(cast<AttributedTypeRepr>(repr), options);
  case TypeReprKind::InOut:
  case TypeReprKind::Shared:
  case TypeReprKind::Owned:
    return resolveSpecifierTypeRepr(cast<SpecifierTypeRepr>(repr), options);

  case TypeReprKind::SimpleIdent:
  case TypeReprKind::GenericIdent:
  case TypeReprKind::CompoundIdent:
    return TC.resolveIdentifierType(DC, cast<IdentTypeRepr>(repr), options,
                                    Resolver);

  case TypeReprKind::Function: {
    if (!(options & TypeResolutionFlags::SILType)) {
      // Default non-escaping for closure parameters
      auto result =
          resolveASTFunctionType(cast<FunctionTypeRepr>(repr), options);
      if (result && result->is<FunctionType>())
        return applyNonEscapingFromContext(DC, result, options);
      return result;
    }
    return resolveSILFunctionType(cast<FunctionTypeRepr>(repr), options);
  }
  case TypeReprKind::SILBox:
    assert((options & TypeResolutionFlags::SILType) && "SILBox repr in non-SIL type context?!");
    return resolveSILBoxType(cast<SILBoxTypeRepr>(repr), options);

  case TypeReprKind::Array:
    return resolveArrayType(cast<ArrayTypeRepr>(repr), options);

  case TypeReprKind::Dictionary:
    return resolveDictionaryType(cast<DictionaryTypeRepr>(repr), options);

  case TypeReprKind::Optional:
    return resolveOptionalType(cast<OptionalTypeRepr>(repr), options);

  case TypeReprKind::ImplicitlyUnwrappedOptional: {
    auto iuoRepr = cast<ImplicitlyUnwrappedOptionalTypeRepr>(repr);
    return resolveImplicitlyUnwrappedOptionalType(iuoRepr, options, isDirect);
  }

  case TypeReprKind::Tuple:
    return resolveTupleType(cast<TupleTypeRepr>(repr), options);

  case TypeReprKind::Composition:
    return resolveCompositionType(cast<CompositionTypeRepr>(repr), options);

  case TypeReprKind::Metatype:
    return resolveMetatypeType(cast<MetatypeTypeRepr>(repr), options);

  case TypeReprKind::Protocol:
    return resolveProtocolType(cast<ProtocolTypeRepr>(repr), options);

  case TypeReprKind::Fixed:
    return cast<FixedTypeRepr>(repr)->getType();
  }
  llvm_unreachable("all cases should be handled");
}

static Type rebuildWithDynamicSelf(ASTContext &Context, Type ty) {
  if (auto metatypeTy = ty->getAs<MetatypeType>()) {
    return MetatypeType::get(
        rebuildWithDynamicSelf(Context, metatypeTy->getInstanceType()),
        metatypeTy->getRepresentation());
  } else if (auto optionalTy = ty->getOptionalObjectType()) {
    return OptionalType::get(rebuildWithDynamicSelf(Context, optionalTy));
  } else {
    return DynamicSelfType::get(ty, Context);
  }
}

Type TypeResolver::resolveAttributedType(AttributedTypeRepr *repr,
                                         TypeResolutionOptions options) {
  // Copy the attributes, since we're about to start hacking on them.
  TypeAttributes attrs = repr->getAttrs();
  assert(!attrs.empty());

  return resolveAttributedType(attrs, repr->getTypeRepr(), options);
}

Type TypeResolver::resolveAttributedType(TypeAttributes &attrs,
                                         TypeRepr *repr,
                                         TypeResolutionOptions options) {
  // Convenience to grab the source range of a type attribute.
  auto getTypeAttrRangeWithAt = [](TypeChecker &TC, SourceLoc attrLoc) {
    return SourceRange(attrLoc.getAdvancedLoc(-1),
                       Lexer::getLocForEndOfToken(TC.Context.SourceMgr,
                                                  attrLoc));

  };

  // Remember whether this is a function parameter.
  bool isParam = options.contains(TypeResolutionFlags::FunctionInput);

  bool isVariadicFunctionParam =
    !options.contains(TypeResolutionFlags::EnumCase) &&
    options.contains(TypeResolutionFlags::VariadicFunctionInput);

  // The type we're working with, in case we want to build it differently
  // based on the attributes we see.
  Type ty;
  
  // In SIL *only*, allow @thin, @thick, or @objc_metatype to apply to
  // a metatype.
  if (attrs.has(TAK_thin) || attrs.has(TAK_thick) || 
      attrs.has(TAK_objc_metatype)) {
    if (auto SF = DC->getParentSourceFile()) {
      if (SF->Kind == SourceFileKind::SIL) {
        TypeRepr *base;
        if (auto metatypeRepr = dyn_cast<MetatypeTypeRepr>(repr)) {
          base = metatypeRepr->getBase();
        } else if (auto protocolRepr = dyn_cast<ProtocolTypeRepr>(repr)) {
          base = protocolRepr->getBase();
        } else {
          base = nullptr;
        }

        if (base) {
          Optional<MetatypeRepresentation> storedRepr;
          // The instance type is not a SIL type.
          auto instanceOptions = options;
          instanceOptions -= TypeResolutionFlags::SILType;
          instanceOptions -= TypeResolutionFlags::FunctionInput;
          instanceOptions -= TypeResolutionFlags::TypeAliasUnderlyingType;

          auto instanceTy = resolveType(base, instanceOptions);
          if (!instanceTy || instanceTy->hasError())
            return instanceTy;

          // Check for @thin.
          if (attrs.has(TAK_thin)) {
            storedRepr = MetatypeRepresentation::Thin;
            attrs.clearAttribute(TAK_thin);
          }

          // Check for @thick.
          if (attrs.has(TAK_thick)) {
            if (storedRepr)
              TC.diagnose(repr->getStartLoc(), 
                          diag::sil_metatype_multiple_reprs);
              
            storedRepr = MetatypeRepresentation::Thick;
            attrs.clearAttribute(TAK_thick);
          }

          // Check for @objc_metatype.
          if (attrs.has(TAK_objc_metatype)) {
            if (storedRepr)
              TC.diagnose(repr->getStartLoc(), 
                          diag::sil_metatype_multiple_reprs);
              
            storedRepr = MetatypeRepresentation::ObjC;
            attrs.clearAttribute(TAK_objc_metatype);
          }

          if (instanceTy->hasError()) {
            ty = instanceTy;
          } else if (auto metatype = dyn_cast<MetatypeTypeRepr>(repr)) {
            ty = buildMetatypeType(metatype, instanceTy, storedRepr);
          } else {
            ty = buildProtocolType(cast<ProtocolTypeRepr>(repr),
                                   instanceTy, storedRepr);
          }
        }
      }
    }
  }

  // Pass down the variable function type attributes to the
  // function-type creator.
  static const TypeAttrKind FunctionAttrs[] = {
    TAK_convention, TAK_noreturn, TAK_pseudogeneric,
    TAK_callee_owned, TAK_callee_guaranteed, TAK_noescape, TAK_autoclosure,
    TAK_escaping, TAK_yield_once, TAK_yield_many
  };

  auto checkUnsupportedAttr = [&](TypeAttrKind attr) {
    if (attrs.has(attr)) {
      TC.diagnose(attrs.getLoc(attr), diag::attribute_not_supported);
      attrs.clearAttribute(attr);
    }
  };
  
  // Some function representation attributes are not supported at source level;
  // only SIL knows how to handle them.  Reject them unless this is a SIL input.
  if (!(options & TypeResolutionFlags::SILType)) {
    for (auto silOnlyAttr : {TAK_callee_owned, TAK_callee_guaranteed}) {
      checkUnsupportedAttr(silOnlyAttr);
    }
  }  

  // Other function representation attributes are not normally supported at
  // source level, but we want to support them there in SIL files.
  auto SF = DC->getParentSourceFile();
  if (!SF || SF->Kind != SourceFileKind::SIL) {
    for (auto silOnlyAttr : {TAK_thin, TAK_thick}) {
      checkUnsupportedAttr(silOnlyAttr);
    }
  }
  
  bool hasFunctionAttr = false;
  for (auto i : FunctionAttrs)
    if (attrs.has(i)) {
      hasFunctionAttr = true;
      break;
    }

  // Function attributes require a syntactic function type.
  auto *fnRepr = dyn_cast<FunctionTypeRepr>(repr);

  if (!fnRepr) {
    if (attrs.has(TAK_autoclosure)) {
      TC.diagnose(attrs.getLoc(TAK_autoclosure),
                  diag::autoclosure_function_type);
      attrs.clearAttribute(TAK_autoclosure);
    }
    // Fall through to diagnose below.
  } else if (hasFunctionAttr && (options & TypeResolutionFlags::SILType)) {
    SILFunctionType::Representation rep;
    TypeRepr *witnessMethodProtocol = nullptr;

    auto coroutineKind = SILCoroutineKind::None;
    if (attrs.has(TAK_yield_once)) {
      coroutineKind = SILCoroutineKind::YieldOnce;
    } else if (attrs.has(TAK_yield_many)) {
      coroutineKind = SILCoroutineKind::YieldMany;
    }

    auto calleeConvention = ParameterConvention::Direct_Unowned;
    if (attrs.has(TAK_callee_owned)) {
      if (attrs.has(TAK_callee_guaranteed)) {
        TC.diagnose(attrs.getLoc(TAK_callee_owned),
                    diag::sil_function_repeat_convention, /*callee*/ 2);
      }
      calleeConvention = ParameterConvention::Direct_Owned;
    } else if (attrs.has(TAK_callee_guaranteed)) {
      calleeConvention = ParameterConvention::Direct_Guaranteed;
    }

    if (!attrs.hasConvention()) {
      rep = SILFunctionType::Representation::Thick;
    } else {
      auto convention = attrs.getConvention();
      // SIL exposes a greater number of conventions than Swift source.
      auto parsedRep =
          llvm::StringSwitch<Optional<SILFunctionType::Representation>>(
              convention)
              .Case("thick", SILFunctionType::Representation::Thick)
              .Case("block", SILFunctionType::Representation::Block)
              .Case("thin", SILFunctionType::Representation::Thin)
              .Case("c", SILFunctionType::Representation::CFunctionPointer)
              .Case("method", SILFunctionType::Representation::Method)
              .Case("objc_method", SILFunctionType::Representation::ObjCMethod)
              .Case("witness_method",
                    SILFunctionType::Representation::WitnessMethod)
              .Default(None);
      if (!parsedRep) {
        TC.diagnose(attrs.getLoc(TAK_convention),
                    diag::unsupported_sil_convention, attrs.getConvention());
        rep = SILFunctionType::Representation::Thin;
      } else {
        rep = *parsedRep;
      }

      if (rep == SILFunctionType::Representation::WitnessMethod) {
        auto protocolName = *attrs.conventionWitnessMethodProtocol;
        witnessMethodProtocol = new (TC.Context) SimpleIdentTypeRepr(
            SourceLoc(), TC.Context.getIdentifier(protocolName));
      }
    }

    // Resolve the function type directly with these attributes.
    SILFunctionType::ExtInfo extInfo(rep, attrs.has(TAK_pseudogeneric),
                                     attrs.has(TAK_noescape));

    ty = resolveSILFunctionType(fnRepr, options, coroutineKind,
                                extInfo, calleeConvention,
                                witnessMethodProtocol);
    if (!ty || ty->hasError()) return ty;
  } else if (hasFunctionAttr) {
    FunctionType::Representation rep = FunctionType::Representation::Swift;
    if (attrs.hasConvention()) {
      auto parsedRep =
        llvm::StringSwitch<Optional<FunctionType::Representation>>
          (attrs.getConvention())
          .Case("swift", FunctionType::Representation::Swift)
          .Case("block", FunctionType::Representation::Block)
          .Case("thin", FunctionType::Representation::Thin)
          .Case("c", FunctionType::Representation::CFunctionPointer)
          .Default(None);
      if (!parsedRep) {
        TC.diagnose(attrs.getLoc(TAK_convention),
                    diag::unsupported_convention, attrs.getConvention());
        rep = FunctionType::Representation::Swift;
      } else {
        rep = *parsedRep;
      }
    }

    // @autoclosure is only valid on parameters.
    if (!isParam && attrs.has(TAK_autoclosure)) {
      TC.diagnose(attrs.getLoc(TAK_autoclosure),
                  isVariadicFunctionParam
                      ? diag::attr_not_on_variadic_parameters
                      : diag::attr_only_on_parameters, "@autoclosure");
      attrs.clearAttribute(TAK_autoclosure);
    }
    
    auto *FuncTyInput = fnRepr->getArgsTypeRepr();
    if ((!FuncTyInput || FuncTyInput->getNumElements() != 0)
        && attrs.has(TAK_autoclosure)) {
      TC.diagnose(attrs.getLoc(TAK_autoclosure),
                  diag::autoclosure_function_input_nonunit);
      attrs.clearAttribute(TAK_autoclosure);
    }

    // @noreturn has been replaced with a 'Never' return type.
    if (attrs.has(TAK_noreturn)) {
      auto loc = attrs.getLoc(TAK_noreturn);
      auto attrRange = getTypeAttrRangeWithAt(TC, loc);
      auto resultRange = fnRepr->getResultTypeRepr()->getSourceRange();

      TC.diagnose(loc, diag::noreturn_not_supported)
          .fixItRemove(attrRange)
          .fixItReplace(resultRange, "Never");
    }

    // Resolve the function type directly with these attributes.
    FunctionType::ExtInfo extInfo(rep,
                                  attrs.has(TAK_autoclosure),
                                  attrs.has(TAK_noescape),
                                  fnRepr->throws());

    ty = resolveASTFunctionType(fnRepr, options, extInfo);
    if (!ty || ty->hasError()) return ty;
  }

  auto instanceOptions = options;
  instanceOptions -= TypeResolutionFlags::FunctionInput;
  instanceOptions -= TypeResolutionFlags::TypeAliasUnderlyingType;

  // If we didn't build the type differently above, we might have
  // a typealias pointing at a function type with the @escaping
  // attribute. Resolve the type as if it were in non-parameter
  // context, and then set isNoEscape if @escaping is not present.
  if (!ty) ty = resolveType(repr, instanceOptions);
  if (!ty || ty->hasError()) return ty;

  // Handle @escaping
  if (hasFunctionAttr && ty->is<FunctionType>()) {
    if (attrs.has(TAK_escaping)) {
      // The attribute is meaningless except on parameter types.
      bool isEnum = options.contains(TypeResolutionFlags::EnumCase);
      if (isEnum || !isParam) {
        auto loc = attrs.getLoc(TAK_escaping);
        auto attrRange = getTypeAttrRangeWithAt(TC, loc);

        TC.diagnose(loc, diag::escaping_non_function_parameter)
            .fixItRemove(attrRange);

        // Try to find a helpful note based on how the type is being used
        if (options.contains(TypeResolutionFlags::ImmediateOptionalTypeArgument)) {
          TC.diagnose(repr->getLoc(), diag::escaping_optional_type_argument);
        }
      }

      attrs.clearAttribute(TAK_escaping);
    } else {
      // No attribute; set the isNoEscape bit if we're in parameter context.
      ty = applyNonEscapingFromContext(DC, ty, options);
    }
  }

  if (hasFunctionAttr && !fnRepr) {
    // @autoclosure usually auto-implies @noescape, don't complain about both
    // of them.
    if (attrs.has(TAK_autoclosure))
      attrs.clearAttribute(TAK_noescape);

    for (auto i : FunctionAttrs) {
      if (!attrs.has(i))
        continue;

      auto diag = TC.diagnose(attrs.getLoc(i),
                              diag::attribute_requires_function_type,
                              TypeAttributes::getAttrName(i));

      // If we see @escaping among the attributes on this type, because it isn't
      // a function type, we'll remove it.
      if (i == TAK_escaping) {
        diag.fixItRemove(getTypeAttrRangeWithAt(TC,
                                                attrs.getLoc(TAK_escaping)));
        // Specialize the diagnostic for Optionals.
        if (ty->getOptionalObjectType()) {
          diag.flush();
          TC.diagnose(repr->getLoc(), diag::escaping_optional_type_argument);
        }
      }
      attrs.clearAttribute(i);
    }
  } else if (hasFunctionAttr && fnRepr) {
    // Remove the function attributes from the set so that we don't diagnose.
    for (auto i : FunctionAttrs)
      attrs.clearAttribute(i);
    attrs.convention = None;
  }

  // In SIL, handle @opened (n), which creates an existential archetype.
  if (attrs.has(TAK_opened)) {
    if (!ty->isExistentialType()) {
      TC.diagnose(attrs.getLoc(TAK_opened), diag::opened_non_protocol, ty);
    } else {
      ty = ArchetypeType::getOpened(ty, attrs.OpenedID);
    }
    attrs.clearAttribute(TAK_opened);
  }

  // In SIL files *only*, permit @weak and @unowned to apply directly to types.
  if (attrs.hasOwnership()) {
    if (auto SF = DC->getParentSourceFile()) {
      if (SF->Kind == SourceFileKind::SIL) {
        if (((attrs.has(TAK_sil_weak) || attrs.has(TAK_sil_unmanaged)) &&
             ty->getOptionalObjectType()) ||
            (!attrs.has(TAK_sil_weak) && ty->hasReferenceSemantics())) {
          ty = ReferenceStorageType::get(ty, attrs.getOwnership(), Context);
          attrs.clearOwnership();
        }
      }
    }
  }
  
  // In SIL *only*, allow @block_storage to specify a block storage type.
  if ((options & TypeResolutionFlags::SILType) && attrs.has(TAK_block_storage)) {
    ty = SILBlockStorageType::get(ty->getCanonicalType());
    attrs.clearAttribute(TAK_block_storage);
  }
  
  // In SIL *only*, allow @box to specify a box type.
  if ((options & TypeResolutionFlags::SILType) && attrs.has(TAK_box)) {
    ty = SILBoxType::get(ty->getCanonicalType());
    attrs.clearAttribute(TAK_box);
  }

  // In SIL *only*, allow @dynamic_self to specify a dynamic Self type.
  if ((options & TypeResolutionFlags::SILMode) && attrs.has(TAK_dynamic_self)) {
    ty = rebuildWithDynamicSelf(TC.Context, ty);
    attrs.clearAttribute(TAK_dynamic_self);
  }

  for (unsigned i = 0; i != TypeAttrKind::TAK_Count; ++i)
    if (attrs.has((TypeAttrKind)i))
      TC.diagnose(attrs.getLoc((TypeAttrKind)i),
                  diag::attribute_does_not_apply_to_type);

  return ty;
}

bool TypeResolver::resolveASTFunctionTypeParams(
    TupleTypeRepr *inputRepr, TypeResolutionOptions options,
    bool requiresMappingOut,
    SmallVectorImpl<AnyFunctionType::Param> &elements) {
  elements.reserve(inputRepr->getNumElements());

  const auto elementOptions = withoutContext(options, true)
                            | TypeResolutionFlags::FunctionInput;
  for (unsigned i = 0, end = inputRepr->getNumElements(); i != end; ++i) {
    auto *eltTypeRepr = inputRepr->getElementType(i);

    // If the element is a variadic parameter, resolve the parameter type as if
    // it were in non-parameter position, since we want functions to be
    // @escaping in this case.
    auto thisElementOptions = elementOptions;
    bool variadic = false;
    if (inputRepr->hasEllipsis() &&
        elements.size() == inputRepr->getEllipsisIndex()) {
      thisElementOptions = withoutContext(elementOptions);
      thisElementOptions |= TypeResolutionFlags::VariadicFunctionInput;
      variadic = true;
    }

    Type ty = resolveType(eltTypeRepr, thisElementOptions);
    if (!ty) return true;

    if (ty->hasError()) {
      elements.emplace_back(ErrorType::get(Context), Identifier(),
                            ParameterTypeFlags());
      continue;
    }

    // Parameters of polymorphic functions speak in terms of interface types.
    if (requiresMappingOut) {
      ty = ty->mapTypeOutOfContext();
    }

    ValueOwnership ownership;
    switch (eltTypeRepr->getKind()) {
    case TypeReprKind::Shared:
      ownership = ValueOwnership::Shared;
      break;
    case TypeReprKind::InOut:
      ownership = ValueOwnership::InOut;
      break;
    case TypeReprKind::Owned:
      ownership = ValueOwnership::Owned;
      break;
    default:
      ownership = ValueOwnership::Default;
      break;
    }
    ParameterTypeFlags paramFlags =
        ParameterTypeFlags::fromParameterType(ty, variadic, ownership);
    elements.emplace_back(ty->getInOutObjectType(), Identifier(), paramFlags);
  }

  return false;
}

Type TypeResolver::resolveASTFunctionType(FunctionTypeRepr *repr,
                                          TypeResolutionOptions options,
                                          FunctionType::ExtInfo extInfo) {
  options -= TypeResolutionFlags::Direct;
  options -= TypeResolutionFlags::FunctionInput;
  options -= TypeResolutionFlags::FunctionResult;
  options -= TypeResolutionFlags::TypeAliasUnderlyingType;

  SmallVector<AnyFunctionType::Param, 8> params;
  if (resolveASTFunctionTypeParams(repr->getArgsTypeRepr(), options,
                         repr->getGenericEnvironment() != nullptr, params)) {
    return Type();
  }

  Type outputTy = resolveType(repr->getResultTypeRepr(), options);
  if (!outputTy || outputTy->hasError()) return outputTy;

  extInfo = extInfo.withThrows(repr->throws());

  // If this is a function type without parens around the parameter list,
  // diagnose this and produce a fixit to add them.
  if (!repr->isWarnedAbout()) {
    // If someone wrote (Void) -> () in Swift 3, they probably meant
    // () -> (), but (Void) -> () is (()) -> () so emit a warning
    // asking if they meant () -> ().
    auto args = repr->getArgsTypeRepr();
    if (args->getNumElements() == 1) {
      if (const auto Void =
          dyn_cast<SimpleIdentTypeRepr>(args->getElementType(0))) {
        if (Void->getIdentifier().str() == "Void") {
          TC.diagnose(args->getStartLoc(), diag::paren_void_probably_void)
            .fixItReplace(args->getSourceRange(), "()");
          repr->setWarned();
        }
      }
    }
  }

  // SIL uses polymorphic function types to resolve overloaded member functions.
  if (auto genericEnv = repr->getGenericEnvironment()) {
    outputTy = outputTy->mapTypeOutOfContext();
    return GenericFunctionType::get(genericEnv->getGenericSignature(),
                                    params, outputTy, extInfo);
  }

  auto fnTy = FunctionType::get(params, outputTy, extInfo);
  // If the type is a block or C function pointer, it must be representable in
  // ObjC.
  switch (auto rep = extInfo.getRepresentation()) {
  case AnyFunctionType::Representation::Block:
  case AnyFunctionType::Representation::CFunctionPointer:
    if (!fnTy->isRepresentableIn(ForeignLanguage::ObjectiveC, DC)) {
      StringRef strName =
        rep == AnyFunctionType::Representation::Block ? "block" : "c";
      auto extInfo2 =
        extInfo.withRepresentation(AnyFunctionType::Representation::Swift);
      auto simpleFnTy = FunctionType::get(params, outputTy, extInfo2);
      TC.diagnose(repr->getStartLoc(), diag::objc_convention_invalid,
                  simpleFnTy, strName);
    }
    break;

  case AnyFunctionType::Representation::Thin:
  case AnyFunctionType::Representation::Swift:
    break;
  }
  
  return fnTy;
}

Type TypeResolver::resolveSILBoxType(SILBoxTypeRepr *repr,
                                     TypeResolutionOptions options) {
  // Resolve the field types.
  SmallVector<SILField, 4> fields;
  {
    // Resolve field types using the box type's generic environment, if it
    // has one. (TODO: Field types should never refer to generic parameters
    // outside the box's own environment; we should really validate that...)
    Optional<GenericTypeToArchetypeResolver>
      resolveSILBoxGenericParams;
    Optional<llvm::SaveAndRestore<GenericTypeResolver*>>
      useSILBoxGenericEnv;
    if (auto env = repr->getGenericEnvironment()) {
      resolveSILBoxGenericParams = GenericTypeToArchetypeResolver(env);
      useSILBoxGenericEnv.emplace(Resolver, &*resolveSILBoxGenericParams);
    }
    
    for (auto &fieldRepr : repr->getFields()) {
      auto fieldTy = resolveType(fieldRepr.getFieldType(), options);
      fields.push_back({fieldTy->getCanonicalType(), fieldRepr.isMutable()});
    }
  }

  // Substitute out parsed context types into interface types.
  CanGenericSignature genericSig;
  if (auto *genericEnv = repr->getGenericEnvironment()) {
    genericSig = genericEnv->getGenericSignature()->getCanonicalSignature();
    
    for (auto &field : fields) {
      auto transTy = field.getLoweredType()->mapTypeOutOfContext();
      field = {transTy->getCanonicalType(), field.isMutable()};
    }
  }
  
  // Resolve the generic arguments.
  // Start by building a TypeSubstitutionMap.
  SubstitutionMap subMap;
  if (genericSig) {
    TypeSubstitutionMap genericArgMap;

    auto params = genericSig->getGenericParams();
    if (repr->getGenericArguments().size()
          != genericSig->getGenericParams().size()) {
      TC.diagnose(repr->getLoc(), diag::sil_box_arg_mismatch);
      return ErrorType::get(Context);
    }
  
    for (unsigned i : indices(params)) {
      auto argTy = resolveType(repr->getGenericArguments()[i], options);
      genericArgMap.insert({params[i], argTy->getCanonicalType()});
    }
    
    bool ok = true;
    subMap = SubstitutionMap::get(
      genericSig,
      QueryTypeSubstitutionMap{genericArgMap},
      [&](CanType depTy, Type replacement, ProtocolDecl *proto)
      -> ProtocolConformanceRef {
        auto result = TC.conformsToProtocol(replacement, proto, DC,
                                            ConformanceCheckOptions());
        // TODO: getSubstitutions callback ought to return Optional.
        if (!result) {
          ok = false;
          return ProtocolConformanceRef(proto);
        }
        
        return *result;
      });

    if (!ok)
      return ErrorType::get(Context);
  }
  
  auto layout = SILLayout::get(Context, genericSig, fields);
  return SILBoxType::get(Context, layout, subMap);
}

Type TypeResolver::resolveSILFunctionType(FunctionTypeRepr *repr,
                                          TypeResolutionOptions options,
                                          SILCoroutineKind coroutineKind,
                                          SILFunctionType::ExtInfo extInfo,
                                          ParameterConvention callee,
                                          TypeRepr *witnessMethodProtocol) {
  options -= TypeResolutionFlags::FunctionInput;
  options -= TypeResolutionFlags::TypeAliasUnderlyingType;

  bool hasError = false;

  // Resolve parameter and result types using the function's generic
  // environment.
  SmallVector<SILParameterInfo, 4> params;
  SmallVector<SILYieldInfo, 4> yields;
  SmallVector<SILResultInfo, 4> results;
  Optional<SILResultInfo> errorResult;
  {
    Optional<GenericTypeToArchetypeResolver>
      resolveSILFunctionGenericParams;
    Optional<llvm::SaveAndRestore<GenericTypeResolver*>>
      useSILFunctionGenericEnv;
    
    // Resolve generic params using the function's generic environment, if it
    // has one.
    if (auto env = repr->getGenericEnvironment()) {
      resolveSILFunctionGenericParams = GenericTypeToArchetypeResolver(env);
      useSILFunctionGenericEnv.emplace(Resolver,
                                       &*resolveSILFunctionGenericParams);
    }
    
    auto argsTuple = repr->getArgsTypeRepr();
    // SIL functions cannot be variadic.
    if (argsTuple->hasEllipsis()) {
      TC.diagnose(argsTuple->getEllipsisLoc(), diag::sil_function_ellipsis);
    }
    // SIL functions cannot have parameter names.
    for (auto &element : argsTuple->getElements()) {
      if (element.UnderscoreLoc.isValid())
        TC.diagnose(element.UnderscoreLoc, diag::sil_function_input_label);
    }

    for (auto elt : argsTuple->getElements()) {
      auto param = resolveSILParameter(elt.Type,
                       options | TypeResolutionFlags::FunctionInput);
      params.push_back(param);
      if (!param.getType()) return nullptr;

      if (param.getType()->hasError())
        hasError = true;
    }

    {
      // FIXME: Deal with unsatisfied dependencies.
      if (resolveSILResults(repr->getResultTypeRepr(), options, yields,
                            results, errorResult)) {
        hasError = true;
      }

      // Diagnose non-coroutines that declare yields.
      if (coroutineKind == SILCoroutineKind::None && !yields.empty()) {
        TC.diagnose(repr->getResultTypeRepr()->getLoc(),
                    diag::sil_non_coro_yields);
        hasError = true;
      }
    }
  } // restore generic type resolver

  if (hasError) {
    return ErrorType::get(Context);
  }

  // FIXME: Remap the parsed context types to interface types.
  CanGenericSignature genericSig;
  SmallVector<SILParameterInfo, 4> interfaceParams;
  SmallVector<SILYieldInfo, 4> interfaceYields;
  SmallVector<SILResultInfo, 4> interfaceResults;
  Optional<SILResultInfo> interfaceErrorResult;
  if (auto *genericEnv = repr->getGenericEnvironment()) {
    genericSig = genericEnv->getGenericSignature()->getCanonicalSignature();
 
    for (auto &param : params) {
      auto transParamType = param.getType()->mapTypeOutOfContext()
          ->getCanonicalType();
      interfaceParams.push_back(param.getWithType(transParamType));
    }
    for (auto &yield : yields) {
      auto transYieldType = yield.getType()->mapTypeOutOfContext()
          ->getCanonicalType();
      interfaceYields.push_back(yield.getWithType(transYieldType));
    }
    for (auto &result : results) {
      auto transResultType = result.getType()->mapTypeOutOfContext()
          ->getCanonicalType();
      interfaceResults.push_back(result.getWithType(transResultType));
    }

    if (errorResult) {
      auto transErrorResultType = errorResult->getType()->mapTypeOutOfContext()
          ->getCanonicalType();
      interfaceErrorResult =
        errorResult->getWithType(transErrorResultType);
    }
  } else {
    interfaceParams = params;
    interfaceYields = yields;
    interfaceResults = results;
    interfaceErrorResult = errorResult;
  }
  Optional<ProtocolConformanceRef> witnessMethodConformance;
  if (witnessMethodProtocol) {
    auto resolved = resolveType(witnessMethodProtocol, options);
    if (resolved->hasError())
      return resolved;

    auto protocolType = resolved->getAs<ProtocolType>();
    if (!protocolType)
      return ErrorType::get(Context);

    Type selfType = params.back().getType();
    // The Self type can be nested in a few layers of metatypes (etc.), e.g. for
    // a mutable static variable the materializeForSet currently has its last
    // argument as a Self.Type.Type metatype.
    while (1) {
      auto next = selfType->getRValueInstanceType();
      if (next->isEqual(selfType))
        break;
      selfType = next;
    }

    witnessMethodConformance = TC.conformsToProtocol(
        selfType, protocolType->getDecl(), DC, ConformanceCheckOptions());
    assert(witnessMethodConformance &&
           "found witness_method without matching conformance");
  }

  return SILFunctionType::get(genericSig, extInfo, coroutineKind,
                              callee,
                              interfaceParams, interfaceYields,
                              interfaceResults, interfaceErrorResult,
                              Context, witnessMethodConformance);
}

SILYieldInfo TypeResolver::resolveSILYield(TypeAttributes &attrs,
                                           TypeRepr *repr,
                                           TypeResolutionOptions options) {
  AttributedTypeRepr attrRepr(attrs, repr);
  SILParameterInfo paramInfo =
    resolveSILParameter(&attrRepr,
                        options | TypeResolutionFlags::FunctionInput);
  return SILYieldInfo(paramInfo.getType(), paramInfo.getConvention());
}

SILParameterInfo TypeResolver::resolveSILParameter(
                                 TypeRepr *repr,
                                 TypeResolutionOptions options) {
  assert(options.contains(TypeResolutionFlags::FunctionInput)
         && "Parameters should be marked as inputs");
  auto convention = DefaultParameterConvention;
  Type type;
  bool hadError = false;

  if (auto attrRepr = dyn_cast<AttributedTypeRepr>(repr)) {
    auto attrs = attrRepr->getAttrs();

    auto checkFor = [&](TypeAttrKind tak, ParameterConvention attrConv) {
      if (!attrs.has(tak)) return;
      if (convention != DefaultParameterConvention) {
        TC.diagnose(attrs.getLoc(tak), diag::sil_function_repeat_convention,
                    /*input*/ 0);
        hadError = true;
      }
      attrs.clearAttribute(tak);
      convention = attrConv;
    };
    checkFor(TypeAttrKind::TAK_in_guaranteed,
             ParameterConvention::Indirect_In_Guaranteed);
    checkFor(TypeAttrKind::TAK_in, ParameterConvention::Indirect_In);
    checkFor(TypeAttrKind::TAK_in_constant,
             ParameterConvention::Indirect_In_Constant);
    checkFor(TypeAttrKind::TAK_inout, ParameterConvention::Indirect_Inout);
    checkFor(TypeAttrKind::TAK_inout_aliasable,
             ParameterConvention::Indirect_InoutAliasable);
    checkFor(TypeAttrKind::TAK_owned, ParameterConvention::Direct_Owned);
    checkFor(TypeAttrKind::TAK_guaranteed,
             ParameterConvention::Direct_Guaranteed);

    type = resolveAttributedType(attrs, attrRepr->getTypeRepr(), options);
  } else {
    type = resolveType(repr, options);
  }

  if (!type || type->hasError()) {
    hadError = true;

  // Diagnose types that are illegal in SIL.
  } else if (!type->isLegalSILType()) {
    TC.diagnose(repr->getLoc(), diag::illegal_sil_type, type);
    hadError = true;
  }

  if (hadError) type = ErrorType::get(Context);
  return SILParameterInfo(type->getCanonicalType(), convention);
}

bool TypeResolver::resolveSingleSILResult(TypeRepr *repr,
                                          TypeResolutionOptions options,
                                          SmallVectorImpl<SILYieldInfo> &yields,
                              SmallVectorImpl<SILResultInfo> &ordinaryResults,
                                       Optional<SILResultInfo> &errorResult) {
  Type type;
  auto convention = DefaultResultConvention;
  bool isErrorResult = false;

  if (auto attrRepr = dyn_cast<AttributedTypeRepr>(repr)) {
    // Copy the attributes out; we're going to destructively modify them.
    auto attrs = attrRepr->getAttrs();

    // Recognize @yields.
    if (attrs.has(TypeAttrKind::TAK_yields)) {
      attrs.clearAttribute(TypeAttrKind::TAK_yields);

      // The treatment from this point on is basically completely different.
      auto yield = resolveSILYield(attrs, attrRepr->getTypeRepr(), options);
      if (yield.getType()->hasError())
        return true;

      yields.push_back(yield);
      return false;
    }

    // Recognize @error.
    if (attrs.has(TypeAttrKind::TAK_error)) {
      attrs.clearAttribute(TypeAttrKind::TAK_error);
      isErrorResult = true;

      // Error results are always implicitly @owned.
      convention = ResultConvention::Owned;
    }

    // Recognize result conventions.
    bool hadError = false;
    auto checkFor = [&](TypeAttrKind tak, ResultConvention attrConv) {
      if (!attrs.has(tak)) return;
      if (convention != DefaultResultConvention) {
        TC.diagnose(attrs.getLoc(tak), diag::sil_function_repeat_convention,
                    /*result*/ 1);
        hadError = true;
      }
      attrs.clearAttribute(tak);
      convention = attrConv;
    };
    checkFor(TypeAttrKind::TAK_out, ResultConvention::Indirect);
    checkFor(TypeAttrKind::TAK_owned, ResultConvention::Owned);
    checkFor(TypeAttrKind::TAK_unowned_inner_pointer,
             ResultConvention::UnownedInnerPointer);
    checkFor(TypeAttrKind::TAK_autoreleased, ResultConvention::Autoreleased);
    if (hadError) return true;

    type = resolveAttributedType(attrs, attrRepr->getTypeRepr(), options);
  } else {
    type = resolveType(repr, options);
  }

  // Propagate type-resolution errors out.
  if (!type || type->hasError()) return true;

  // Diagnose types that are illegal in SIL.
  if (!type->isLegalSILType()) {
    TC.diagnose(repr->getStartLoc(), diag::illegal_sil_type, type);
    return false;
  }

  assert(!isErrorResult || convention == ResultConvention::Owned);
  SILResultInfo resolvedResult(type->getCanonicalType(), convention);

  if (!isErrorResult) {
    ordinaryResults.push_back(resolvedResult);
    return false;
  }

  // Error result types must have pointer-like representation.
  // FIXME: check that here?

  // We don't expect to have a reason to support multiple independent
  // error results.  (Would this be disjunctive or conjunctive?)
  if (errorResult.hasValue()) {
    TC.diagnose(repr->getStartLoc(),
                diag::sil_function_multiple_error_results);
    return true;
  }

  errorResult = resolvedResult;
  return false;
}

bool TypeResolver::resolveSILResults(TypeRepr *repr,
                                     TypeResolutionOptions options,
                                SmallVectorImpl<SILYieldInfo> &yields,
                                SmallVectorImpl<SILResultInfo> &ordinaryResults,
                                Optional<SILResultInfo> &errorResult) {
  if (auto tuple = dyn_cast<TupleTypeRepr>(repr)) {
    bool hadError = false;
    for (auto &element : tuple->getElements()) {
      if (element.UnderscoreLoc.isValid())
        TC.diagnose(element.UnderscoreLoc, diag::sil_function_output_label);
    }
    for (auto elt : tuple->getElements()) {
      if (resolveSingleSILResult(elt.Type, options,
                                 yields, ordinaryResults, errorResult))
        hadError = true;
    }
    return hadError;
  }

  return resolveSingleSILResult(repr, options,
                                yields, ordinaryResults, errorResult);
}

Type TypeResolver::resolveSpecifierTypeRepr(SpecifierTypeRepr *repr,
                                            TypeResolutionOptions options) {
  // inout is only valid for (non-Subscript and non-EnumCaseDecl)
  // function parameters.
  if ((options & TypeResolutionFlags::SubscriptParameters) ||
      (options & TypeResolutionFlags::EnumCase) ||
        (!(options & TypeResolutionFlags::FunctionInput))) {

    decltype(diag::attr_only_on_parameters) diagID;
    if (options & TypeResolutionFlags::SubscriptParameters) {
      diagID = diag::attr_not_on_subscript_parameters;
    } else if (options & TypeResolutionFlags::VariadicFunctionInput) {
      diagID = diag::attr_not_on_variadic_parameters;
    } else {
      diagID = diag::attr_only_on_parameters;
    }
    StringRef name;
    switch (repr->getKind()) {
    case TypeReprKind::InOut:
      name = "inout";
      break;
    case TypeReprKind::Shared:
      name = "__shared";
      break;
    case TypeReprKind::Owned:
      name = "__owned";
      break;
    default:
      llvm_unreachable("unknown SpecifierTypeRepr kind");
    }
    TC.diagnose(repr->getSpecifierLoc(), diagID, name);
    repr->setInvalid();
    return ErrorType::get(Context);
  }

  if (!isa<ImplicitlyUnwrappedOptionalTypeRepr>(repr->getBase())) {
    // Anything within the inout isn't a parameter anymore.
    options -= TypeResolutionFlags::FunctionInput;
    options -= TypeResolutionFlags::TypeAliasUnderlyingType;
  }

  Type ty = resolveType(repr->getBase(), options);
  if (!ty || ty->hasError()) return ty;
  if (repr->getKind() == TypeReprKind::InOut) return InOutType::get(ty);
  return ty;
}


Type TypeResolver::resolveArrayType(ArrayTypeRepr *repr,
                                    TypeResolutionOptions options) {
  // FIXME: diagnose non-materializability of element type!
  Type baseTy = resolveType(repr->getBase(), withoutContext(options));
  if (!baseTy || baseTy->hasError()) return baseTy;

  auto sliceTy = TC.getArraySliceType(repr->getBrackets().Start, baseTy);
  if (!sliceTy)
    return ErrorType::get(Context);

  if (!options.contains(TypeResolutionFlags::ResolveStructure)) {
    // Check for _ObjectiveCBridgeable conformances in the element type.
    useObjectiveCBridgeableConformances(DC, baseTy);
  }

  return sliceTy;
}

Type TypeResolver::resolveDictionaryType(DictionaryTypeRepr *repr,
                                         TypeResolutionOptions options) {
  options = adjustOptionsForGenericArgs(options);

  // FIXME: diagnose non-materializability of key/value type?
  Type keyTy = resolveType(repr->getKey(), withoutContext(options));
  if (!keyTy || keyTy->hasError()) return keyTy;

  Type valueTy = resolveType(repr->getValue(), withoutContext(options));
  if (!valueTy || valueTy->hasError()) return valueTy;

  auto dictDecl = TC.Context.getDictionaryDecl();

  if (auto dictTy = TC.getDictionaryType(repr->getBrackets().Start, keyTy, 
                                         valueTy)) {
    // Check the requirements on the generic arguments.
    auto unboundTy = dictDecl->getDeclaredType()->castTo<UnboundGenericType>();

    if (!options.contains(TypeResolutionFlags::ResolveStructure)) {
      Type args[] = {keyTy, valueTy};

      if (!TC.applyUnboundGenericArguments(
              unboundTy, dictDecl, repr->getStartLoc(), DC, args,
              Resolver)) {
        return nullptr;
      }

      // Check for _ObjectiveCBridgeable conformances in the key and value
      // types.
      useObjectiveCBridgeableConformances(DC, keyTy);
      useObjectiveCBridgeableConformances(DC, valueTy);
    }

    return dictTy;
  }

  return ErrorType::get(Context);
}

Type TypeResolver::resolveOptionalType(OptionalTypeRepr *repr,
                                       TypeResolutionOptions options) {
  auto elementOptions = withoutContext(options, true);
  elementOptions |= TypeResolutionFlags::ImmediateOptionalTypeArgument;

  // The T in T? is a generic type argument and therefore always an AST type.
  // FIXME: diagnose non-materializability of element type!
  Type baseTy = resolveType(repr->getBase(), elementOptions);
  if (!baseTy || baseTy->hasError()) return baseTy;

  auto optionalTy = TC.getOptionalType(repr->getQuestionLoc(), baseTy);
  if (!optionalTy) return ErrorType::get(Context);

  return optionalTy;
}

Type TypeResolver::resolveImplicitlyUnwrappedOptionalType(
      ImplicitlyUnwrappedOptionalTypeRepr *repr,
      TypeResolutionOptions options,
      bool isDirect) {
  TypeResolutionOptions allowIUO = TypeResolutionFlags::SILType;
  allowIUO |= TypeResolutionFlags::FunctionInput;
  allowIUO |= TypeResolutionFlags::FunctionResult;
  allowIUO |= TypeResolutionFlags::PatternBindingEntry;

  if (!isDirect || !(options & allowIUO)) {
    // Prior to Swift 5, we allow 'as T!' and turn it into a disjunction.
    if (TC.Context.isSwiftVersionAtLeast(5)) {
      TC.diagnose(repr->getStartLoc(),
                  diag::implicitly_unwrapped_optional_in_illegal_position)
          .fixItReplace(repr->getExclamationLoc(), "?");
    } else if (options.contains(TypeResolutionFlags::InCastOrCoercionExpression)) {
      TC.diagnose(
          repr->getStartLoc(),
          diag::implicitly_unwrapped_optional_deprecated_in_this_position);
    } else {
      TC.diagnose(
          repr->getStartLoc(),
          diag::implicitly_unwrapped_optional_in_illegal_position_interpreted_as_optional)
          .fixItReplace(repr->getExclamationLoc(), "?");
    }
  }

  auto elementOptions = withoutContext(options, true);
  elementOptions |= TypeResolutionFlags::ImmediateOptionalTypeArgument;

  // The T in T! is a generic type argument and therefore always an AST type.
  // FIXME: diagnose non-materializability of element type!
  Type baseTy = resolveType(repr->getBase(), elementOptions);
  if (!baseTy || baseTy->hasError()) return baseTy;

  Type uncheckedOptionalTy;
  uncheckedOptionalTy = TC.getOptionalType(repr->getExclamationLoc(), baseTy);

  if (!uncheckedOptionalTy)
    return ErrorType::get(Context);

  return uncheckedOptionalTy;
}

Type TypeResolver::resolveTupleType(TupleTypeRepr *repr,
                                    TypeResolutionOptions options) {
  SmallVector<TupleTypeElt, 8> elements;
  elements.reserve(repr->getNumElements());
  
  auto elementOptions = options;
  if (repr->isParenType()) {
    elementOptions -= TypeResolutionFlags::Direct;
  } else {
    elementOptions = withoutContext(elementOptions, true);
  }

  // Variadic tuples are not permitted.
  bool complained = false;
  if (repr->hasEllipsis()) {
    TC.diagnose(repr->getEllipsisLoc(), diag::tuple_ellipsis);
    repr->removeEllipsis();
    complained = true;
  }

  for (unsigned i = 0, end = repr->getNumElements(); i != end; ++i) {
    auto *tyR = repr->getElementType(i);

    Type ty = resolveType(tyR, elementOptions);
    if (!ty || ty->hasError()) return ty;

    elements.emplace_back(ty->getInOutObjectType(),
                          repr->getElementName(i), ParameterTypeFlags());
  }

  // Single-element labeled tuples are not permitted outside of declarations
  // or SIL, either.
  if (elements.size() == 1 && elements[0].hasName()
      && !(options & TypeResolutionFlags::SILType)) {
    if (!complained) {
      TC.diagnose(repr->getElementNameLoc(0),
                  diag::tuple_single_element)
        .fixItRemoveChars(repr->getElementNameLoc(0),
                          repr->getElementType(0)->getStartLoc());
    }

    elements[0] = TupleTypeElt(elements[0].getType());
  }

  return TupleType::get(elements, Context);
}

Type TypeResolver::resolveCompositionType(CompositionTypeRepr *repr,
                                          TypeResolutionOptions options) {

  // Note that the superclass type will appear as part of one of the
  // types in 'Members', so it's not used when constructing the
  // fully-realized type below -- but we just record it to make sure
  // there is only one superclass.
  Type SuperclassType;
  SmallVector<Type, 4> Members;

  // Whether we saw at least one protocol. A protocol composition
  // must either be empty (in which case it is Any or AnyObject),
  // or if it has a superclass constraint, have at least one protocol.
  bool HasProtocol = false;

  auto checkSuperclass = [&](SourceLoc loc, Type t) -> bool {
    if (SuperclassType && !SuperclassType->isEqual(t)) {
      TC.diagnose(loc, diag::protocol_composition_one_class, t,
                  SuperclassType);
      return true;
    }

    SuperclassType = t;
    return false;
  };

  for (auto tyR : repr->getTypes()) {
    Type ty = resolveType(tyR, withoutContext(options));
    if (!ty || ty->hasError()) return ty;

    auto nominalDecl = ty->getAnyNominal();
    if (nominalDecl && isa<ClassDecl>(nominalDecl)) {
      if (checkSuperclass(tyR->getStartLoc(), ty))
        continue;

      Members.push_back(ty);
      continue;
    }

    if (ty->isExistentialType()) {
      auto layout = ty->getExistentialLayout();
      if (auto superclass = layout.explicitSuperclass)
        if (checkSuperclass(tyR->getStartLoc(), superclass))
          continue;
      if (!layout.getProtocols().empty())
        HasProtocol = true;

      Members.push_back(ty);
      continue;
    }

    TC.diagnose(tyR->getStartLoc(),
                diag::invalid_protocol_composition_member,
                ty);
  }

  // Avoid confusing diagnostics ('MyClass' not convertible to 'MyClass',
  // etc) by collapsing a composition consisting of a single class down
  // to the class itself.
  if (SuperclassType && !HasProtocol)
    return SuperclassType;

  // In user-written types, AnyObject constraints always refer to the
  // AnyObject type in the standard library.
  return ProtocolCompositionType::get(Context, Members,
                                      /*HasExplicitAnyObject=*/false);
}

Type TypeResolver::resolveMetatypeType(MetatypeTypeRepr *repr,
                                       TypeResolutionOptions options) {
  // The instance type of a metatype is always abstract, not SIL-lowered.
  Type ty = resolveType(repr->getBase(), withoutContext(options));
  if (!ty || ty->hasError()) return ty;

  Optional<MetatypeRepresentation> storedRepr;
  
  // In SIL mode, a metatype must have a @thin, @thick, or
  // @objc_metatype attribute, so metatypes should have been lowered
  // in resolveAttributedType.
  if (options & TypeResolutionFlags::SILType) {
    TC.diagnose(repr->getStartLoc(), diag::sil_metatype_without_repr);
    storedRepr = MetatypeRepresentation::Thick;
  }

  return buildMetatypeType(repr, ty, storedRepr);
}

Type TypeResolver::buildMetatypeType(
       MetatypeTypeRepr *repr,
       Type instanceType,
       Optional<MetatypeRepresentation> storedRepr) {
  if (instanceType->isAnyExistentialType()) {
    // TODO: diagnose invalid representations?
    return ExistentialMetatypeType::get(instanceType, storedRepr);
  } else {
    return MetatypeType::get(instanceType, storedRepr);
  }
}

Type TypeResolver::resolveProtocolType(ProtocolTypeRepr *repr,
                                       TypeResolutionOptions options) {
  // The instance type of a metatype is always abstract, not SIL-lowered.
  Type ty = resolveType(repr->getBase(), withoutContext(options));
  if (!ty || ty->hasError()) return ty;

  Optional<MetatypeRepresentation> storedRepr;
  
  // In SIL mode, a metatype must have a @thin, @thick, or
  // @objc_metatype attribute, so metatypes should have been lowered
  // in resolveAttributedType.
  if (options & TypeResolutionFlags::SILType) {
    TC.diagnose(repr->getStartLoc(), diag::sil_metatype_without_repr);
    storedRepr = MetatypeRepresentation::Thick;
  }

  return buildProtocolType(repr, ty, storedRepr);
}

Type TypeResolver::buildProtocolType(
       ProtocolTypeRepr *repr,
       Type instanceType,
       Optional<MetatypeRepresentation> storedRepr) {
  if (!instanceType->isAnyExistentialType()) {
    TC.diagnose(repr->getProtocolLoc(), diag::dot_protocol_on_non_existential,
                instanceType);
    return ErrorType::get(TC.Context);
  }

  return MetatypeType::get(instanceType, storedRepr);
}

Type TypeChecker::substMemberTypeWithBase(ModuleDecl *module,
                                          TypeDecl *member,
                                          Type baseTy,
                                          bool useArchetypes) {
  Type sugaredBaseTy = baseTy;

  // For type members of a base class, make sure we use the right
  // derived class as the parent type.
  if (auto *ownerClass = member->getDeclContext()
          ->getAsClassOrClassExtensionContext()) {
    baseTy = baseTy->getSuperclassForDecl(ownerClass, useArchetypes);
  }

  if (baseTy->is<ModuleType>()) {
    baseTy = Type();
    sugaredBaseTy = Type();
  }

  // The declared interface type for a generic type will have the type
  // arguments; strip them off.
  if (auto *nominalDecl = dyn_cast<NominalTypeDecl>(member)) {
    // If the base type is not a nominal type, we might be looking up a
    // nominal member of a generic parameter. This is not supported right
    // now, but at least don't crash.
    if (member->getDeclContext()->getAsProtocolOrProtocolExtensionContext())
      return nominalDecl->getDeclaredType();

    if (!isa<ProtocolDecl>(nominalDecl) &&
        nominalDecl->getGenericParams()) {
      return UnboundGenericType::get(
          nominalDecl, baseTy,
          nominalDecl->getASTContext());
    }
    
    return NominalType::get(
        nominalDecl, baseTy,
        nominalDecl->getASTContext());
  }

  auto *aliasDecl = dyn_cast<TypeAliasDecl>(member);
  if (aliasDecl) {
    // FIXME: If this is a protocol typealias and we haven't built the
    // protocol's generic environment yet, do so now, to ensure the
    // typealias's underlying type has fully resolved dependent
    // member types.
    if (auto *protoDecl = dyn_cast<ProtocolDecl>(aliasDecl->getDeclContext()))
      if (protoDecl->getGenericEnvironment() == nullptr)
        validateDecl(protoDecl);

    if (aliasDecl->getGenericParams()) {
      return UnboundGenericType::get(
          aliasDecl, baseTy,
          aliasDecl->getASTContext());
    }
  }

  Type resultType;
  auto memberType = aliasDecl ? aliasDecl->getUnderlyingTypeLoc().getType()
                              : member->getDeclaredInterfaceType();
  SubstitutionMap subs;
  if (baseTy) {
    // Cope with the presence of unbound generic types, which are ill-formed
    // at this point but break the invariants of getContextSubstitutionMap().
    if (baseTy->hasUnboundGenericType()) {
      if (memberType->hasTypeParameter())
        return ErrorType::get(memberType);

      return memberType;
    }

    subs = baseTy->getContextSubstitutionMap(module, member->getDeclContext());
    resultType = memberType.subst(subs, SubstFlags::UseErrorType);
  } else {
    resultType = memberType;
  }

  // If we're referring to a typealias within a generic context, build
  // a sugared alias type.
  if (aliasDecl && (!sugaredBaseTy || !sugaredBaseTy->isAnyExistentialType())) {
    resultType = NameAliasType::get(aliasDecl, sugaredBaseTy, subs,
                                         resultType);
  }

  return resultType;
}

Type TypeChecker::getSuperClassOf(Type type) {
  if (auto *parenTy = dyn_cast<ParenType>(type.getPointer())) {
    auto superclassTy = getSuperClassOf(parenTy->getUnderlyingType());
    if (!superclassTy)
      return Type();
    return ParenType::get(Context, superclassTy);
  }
  return type->getSuperclass();
}

namespace {

class UnsupportedProtocolVisitor
  : public TypeReprVisitor<UnsupportedProtocolVisitor>, public ASTWalker
{
  TypeChecker &TC;
  bool checkStatements;
  bool hitTopStmt;
    
public:
  UnsupportedProtocolVisitor(TypeChecker &tc, bool checkStatements)
    : TC(tc), checkStatements(checkStatements), hitTopStmt(false) { }

  bool walkToTypeReprPre(TypeRepr *T) override {
    if (T->isInvalid())
      return false;
    if (auto compound = dyn_cast<CompoundIdentTypeRepr>(T)) {
      // Only visit the last component to check, because nested typealiases in
      // existentials are okay.
      visit(compound->getComponentRange().back());
      return false;
    }
    visit(T);
    return true;
  }

  std::pair<bool, Stmt*> walkToStmtPre(Stmt *S) override {
    if (checkStatements && !hitTopStmt) {
      hitTopStmt = true;
      return { true, S };
    }

    return { false, S };
  }

  bool walkToDeclPre(Decl *D) override {
    return !checkStatements;
  }

  void visitIdentTypeRepr(IdentTypeRepr *T) {
    if (T->isInvalid())
      return;
    
    auto comp = T->getComponentRange().back();
    if (auto *proto = dyn_cast_or_null<ProtocolDecl>(comp->getBoundDecl())) {
      if (!proto->existentialTypeSupported(&TC)) {
        TC.diagnose(comp->getIdLoc(), diag::unsupported_existential_type,
                    proto->getName());
        T->setInvalid();
      }
    } else if (auto *alias = dyn_cast_or_null<TypeAliasDecl>(comp->getBoundDecl())) {
      if (!alias->hasInterfaceType())
        return;
      auto type = Type(alias->getDeclaredInterfaceType()->getDesugaredType());
      type.findIf([&](Type type) -> bool {
        if (T->isInvalid())
          return false;
        if (type->isExistentialType()) {
          auto layout = type->getExistentialLayout();
          for (auto *proto : layout.getProtocols()) {
            auto *protoDecl = proto->getDecl();

            if (protoDecl->existentialTypeSupported(&TC))
              continue;
            
            TC.diagnose(comp->getIdLoc(), diag::unsupported_existential_type,
                        protoDecl->getName());
            T->setInvalid();
          }
        }
        return false;
      });
    }
  }
};

} // end anonymous namespace

void TypeChecker::checkUnsupportedProtocolType(Decl *decl) {
  if (!decl || decl->isInvalid())
    return;

  // Type declarations are okay.
  if (isa<TypeDecl>(decl))
    return;

  // Extensions are okay.
  if (isa<ExtensionDecl>(decl))
    return;

  UnsupportedProtocolVisitor visitor(*this, /*checkStatements=*/false);
  decl->walk(visitor);
}

void TypeChecker::checkUnsupportedProtocolType(Stmt *stmt) {
  if (!stmt)
    return;

  UnsupportedProtocolVisitor visitor(*this, /*checkStatements=*/true);
  stmt->walk(visitor);
}
