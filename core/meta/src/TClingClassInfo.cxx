// @(#)root/core/meta:$Id$
// Author: Paul Russo   30/07/2012

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// TClingClassInfo                                                      //
//                                                                      //
// Emulation of the CINT ClassInfo class.                               //
//                                                                      //
// The CINT C++ interpreter provides an interface to metadata about     //
// a class through the ClassInfo class.  This class provides the same   //
// functionality, using an interface as close as possible to ClassInfo  //
// but the class metadata comes from the Clang C++ compiler, not CINT.  //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

#include "TClingClassInfo.h"

#include "TClassEdit.h"
#include "TClingBaseClassInfo.h"
#include "TClingMethodInfo.h"
#include "TDictionary.h"
#include "TClingTypeInfo.h"
#include "TError.h"
#include "TMetaUtils.h"

#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/LookupHelper.h"
#include "cling/Interpreter/StoredValueRef.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Type.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Sema/Sema.h"

#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <sstream>
#include <string>

using namespace clang;
using namespace ROOT;

static std::string FullyQualifiedName(const Decl *decl) {
   // Return the fully qualified name without worrying about normalizing it.
   std::string buf;
   if (const NamedDecl* ND = llvm::dyn_cast<NamedDecl>(decl)) {
      PrintingPolicy Policy(decl->getASTContext().getPrintingPolicy());
      llvm::raw_string_ostream stream(buf);
      ND->getNameForDiagnostic(stream, Policy, /*Qualified=*/true);
   }
   return buf;
}

TClingClassInfo::TClingClassInfo(cling::Interpreter *interp)
   : fInterp(interp), fFirstTime(true), fDescend(false), fDecl(0), fType(0)
{
   TranslationUnitDecl *TU =
      interp->getCI()->getASTContext().getTranslationUnitDecl();
   // Could trigger deserialization of decls.
   cling::Interpreter::PushTransactionRAII RAII(interp);
   fIter = TU->decls_begin();
   InternalNext();
   fFirstTime = true;
   // CINT had this odd behavior where a ClassInfo created without any
   // argument/input was set as an iterator that was ready to be iterated
   // on but was set an not IsValid *BUT* a few routine where using this
   // state as representing the global namespace (These routines include the
   // GetMethod routines and CallFunc::SetFunc, but do not include many others
   // (such as Property etc).  To be somewhat backward compatible, let's make
   // this state actually valid (i.e., representing both the ready-for-first-
   // iteration iterator *and* the global namespace) so that code that was
   // working with CINT (grabbing the default initialized ClassInfo
   // to look at the global namespace) is working again (and, yes, things that
   // used to not work like 'asking' the filename on this will go 'further'
   // but oh well).
   fDecl = TU;
   fType = 0;
}

TClingClassInfo::TClingClassInfo(cling::Interpreter *interp, const char *name)
   : fInterp(interp), fFirstTime(true), fDescend(false), fDecl(0), fType(0),
     fTitle("")
{
   const cling::LookupHelper& lh = fInterp->getLookupHelper();
   const Type *type = 0;
   const Decl *decl = lh.findScope(name,&type, /* intantiateTemplate= */ true );
   if (!decl) {
      std::string buf = TClassEdit::InsertStd(name);
      decl = lh.findScope(buf,&type, /* intantiateTemplate= */ true );
   }
   if (!decl && type) {
      const TagType *tagtype =type->getAs<TagType>();
      if (tagtype) {
         decl = tagtype->getDecl();
      }
   }
   fDecl = decl;
   fType = type;
}

TClingClassInfo::TClingClassInfo(cling::Interpreter *interp,
                                 const Type &tag)
   : fInterp(interp), fFirstTime(true), fDescend(false), fDecl(0), fType(0), 
     fTitle("")
{
   Init(tag);
}

long TClingClassInfo::ClassProperty() const
{
   if (!IsValid()) {
      return 0L;
   }
   long property = 0L;
   const RecordDecl *RD = llvm::dyn_cast<RecordDecl>(fDecl);
   if (!RD) {
      // We are an enum or namespace.
      // The cint interface always returns 0L for these guys.
      return property;
   }
   if (RD->isUnion()) {
      // The cint interface always returns 0L for these guys.
      return property;
   }
   // We now have a class or a struct.
   const CXXRecordDecl *CRD =
      llvm::dyn_cast<CXXRecordDecl>(fDecl);
   property |= kClassIsValid;
   if (CRD->isAbstract()) {
      property |= kClassIsAbstract;
   }
   if (CRD->hasUserDeclaredConstructor()) {
      property |= kClassHasExplicitCtor;
   }
   if (
      !CRD->hasUserDeclaredConstructor() &&
      !CRD->hasTrivialDefaultConstructor()
   ) {
      property |= kClassHasImplicitCtor;
   }
   if (
      CRD->hasUserProvidedDefaultConstructor() ||
      !CRD->hasTrivialDefaultConstructor()
   ) {
      property |= kClassHasDefaultCtor;
   }
   if (CRD->hasUserDeclaredDestructor()) {
      property |= kClassHasExplicitDtor;
   }
   else if (!CRD->hasTrivialDestructor()) {
      property |= kClassHasImplicitDtor;
   }
   if (CRD->hasUserDeclaredCopyAssignment()) {
      property |= kClassHasAssignOpr;
   }
   if (CRD->isPolymorphic()) {
      property |= kClassHasVirtual;
   }
   return property;
}

void TClingClassInfo::Delete(void *arena) const
{
   // Invoke operator delete on a pointer to an object
   // of this class type.
   if (!IsLoaded()) {
      return;
   }
   std::ostringstream os;
   os << "delete (" << FullyQualifiedName(fDecl)  << "*)"
      << reinterpret_cast<unsigned long>(arena) << ";";
   cling::Interpreter::CompilationResult err =
      fInterp->execute(os.str());
   if (err != cling::Interpreter::kSuccess) {
      return;
   }
   return;
}

void TClingClassInfo::DeleteArray(void *arena, bool dtorOnly) const
{
   // Invoke operator delete[] on a pointer to an array object
   // of this class type.
   if (!IsLoaded()) {
      return;
   }
   if (dtorOnly) {
      // There is no syntax in C++ for invoking the placement delete array
      // operator, so we have to placement delete each element by hand.
      // Unfortunately we do not know how many elements to delete.
      Error("DeleteArray", "Placement delete of an array is unsupported!\n");
   }
   else {
      std::ostringstream os;
      os << "delete[] (" << FullyQualifiedName(fDecl)  << "*)"
         << reinterpret_cast<unsigned long>(arena) << ";";
      cling::Interpreter::CompilationResult err =
         fInterp->execute(os.str());
      if (err != cling::Interpreter::kSuccess) {
         return;
      }
   }
   return;
}

void TClingClassInfo::Destruct(void *arena) const
{
   // Invoke placement operator delete on a pointer to an array object
   // of this class type.
   if (!IsLoaded()) {
      return;
   }
   const NamedDecl* ND = dyn_cast<NamedDecl>(fDecl);
   if (!ND || !ND->getIdentifier()) {
      Error("TClingClassInfo::Destruct",
            "cannot destruct object of unnamed declaration.");
      return;
   }
   
   std::string name( FullyQualifiedName(fDecl) );
   std::ostringstream os;
   // compose "((Nsp1::Nsp2::C*)123456)->Nsp1::Nsp2::C::~C();
   os << "((" << name << "*)" << reinterpret_cast<unsigned long>(arena)
      << ")->" << name << "::~" << ND->getNameAsString() << "();";
   cling::Interpreter::CompilationResult err =
      fInterp->execute(os.str());
   if (err != cling::Interpreter::kSuccess) {
      return;
   }
   return;
}

TClingMethodInfo TClingClassInfo::GetMethod(const char *fname,
      const char *proto, long *poffset, EFunctionMatchMode mode /*= kConversionMatch*/,
      InheritanceMode imode /*= WithInheritance*/) const
{
   return GetMethod(fname,proto,false,poffset,mode,imode);
}

TClingMethodInfo TClingClassInfo::GetMethod(const char *fname,
      const char *proto, bool objectIsConst,
      long *poffset, EFunctionMatchMode mode /*= kConversionMatch*/,
      InheritanceMode imode /*= WithInheritance*/) const
{
   if (poffset) {
      *poffset = 0L;
   }
   if (!IsLoaded()) {
      TClingMethodInfo tmi(fInterp);
      return tmi;
   }
   const TagDecl *TD = llvm::dyn_cast<TagDecl>(fDecl);
   if (TD && !TD->isEnum() && !strcmp(fname, Name())) {
      // Constructor.
      // These must be accessed through a wrapper, since they
      // may call an operator new(), which can be a member
      // function or a global function, as well as the actual
      // constructor function itself.
   }
   const cling::LookupHelper& lh = fInterp->getLookupHelper();
   const FunctionDecl *fd;
   if (mode == kConversionMatch) {
      fd = lh.findFunctionProto(fDecl, fname, proto, objectIsConst);
   } else if (mode == kExactMatch) {
      fd = lh.matchFunctionProto(fDecl, fname, proto, objectIsConst);
   } else {
      Error("TClingClassInfo::GetMethod",
            "The MatchMode %d is not supported.", mode);
      TClingMethodInfo tmi(fInterp);
      return tmi;      
   }
   if (!fd) {
      // Function not found.
      TClingMethodInfo tmi(fInterp);
      return tmi;
   }
   if (poffset) {
     // We have been asked to return a this pointer adjustment.
     if (const CXXMethodDecl *md =
           llvm::dyn_cast<CXXMethodDecl>(fd)) {
        // This is a class member function.
        *poffset = GetOffset(md);
     }
   }
   TClingMethodInfo tmi(fInterp);
   tmi.Init(fd);
   return tmi;
}

TClingMethodInfo TClingClassInfo::GetMethodWithArgs(const char *fname,
      const char *arglist, long *poffset, EFunctionMatchMode mode /* = kConversionMatch*/,
      InheritanceMode imode /* = WithInheritance*/) const
{
   return GetMethodWithArgs(fname,arglist,false,poffset,mode,imode);
}

TClingMethodInfo TClingClassInfo::GetMethodWithArgs(const char *fname,
      const char *arglist, bool objectIsConst,
      long *poffset, EFunctionMatchMode /*mode = kConversionMatch*/,
      InheritanceMode /* imode = WithInheritance*/) const
{
   if (poffset) {
      *poffset = 0L;
   }
   if (!IsLoaded()) {
      TClingMethodInfo tmi(fInterp);
      return tmi;
   }
   if (!strcmp(arglist, ")")) {
      // CINT accepted a single right paren as meaning no arguments.
      arglist = "";
   }
   const TagDecl *TD = llvm::dyn_cast<TagDecl>(fDecl);
   if (TD && !TD->isEnum() && !strcmp(fname, Name())) {
      // Constructor.
      // These must be accessed through a wrapper, since they
      // may call an operator new(), which can be a member
      // function or a global function, as well as the actual
      // constructor function itself.
   }
   const cling::LookupHelper &lh = fInterp->getLookupHelper();
   const FunctionDecl *fd = lh.findFunctionArgs(fDecl, fname, arglist, objectIsConst);
   if (!fd) {
      // Function not found.
      TClingMethodInfo tmi(fInterp);
      return tmi;
   }
   if (poffset) {
     // We have been asked to return a this pointer adjustment.
     if (const CXXMethodDecl *md =
           llvm::dyn_cast<CXXMethodDecl>(fd)) {
        // This is a class member function.
        *poffset = GetOffset(md);
     }
   }
   TClingMethodInfo tmi(fInterp);
   tmi.Init(fd);
   return tmi;
}

int TClingClassInfo::GetMethodNArg(const char *method, const char *proto,
                                   Bool_t objectIsConst,
                                   EFunctionMatchMode mode /*= kConversionMatch*/) const
{
   // Note: Used only by TQObject.cxx:170 and only for interpreted classes.
   if (!IsLoaded()) {
      return -1;
   }
   TClingMethodInfo mi = GetMethod(method, proto, objectIsConst, 0, mode);
   int clang_val = -1;
   if (mi.IsValid()) {
      unsigned num_params = mi.GetMethodDecl()->getNumParams();
      clang_val = static_cast<int>(num_params);
   }
   return clang_val;
}

long TClingClassInfo::GetOffset(const CXXMethodDecl* md) const
{
   long offset = 0L;
   const CXXRecordDecl* definer = md->getParent();
   const CXXRecordDecl* accessor =
      llvm::cast<CXXRecordDecl>(fDecl);
   if (definer != accessor) {
      // This function may not be accessible using a pointer
      // to the declaring class, get the adjustment necessary
      // to convert that to a pointer to the defining class.
      TClingBaseClassInfo bi(fInterp, const_cast<TClingClassInfo*>(this));
      while (bi.Next(0)) {
         TClingClassInfo* bci = bi.GetBase();
         if (bci->GetDecl() == definer) {
            // We have found the right base class, now get the
            // necessary adjustment.
            offset = bi.Offset();
            break;
         }
      }
   }
   return offset;
}

bool TClingClassInfo::HasDefaultConstructor() const
{
   // Return true if there a public constructor taking no argument
   // (including a constructor that has defaults for all its arguments).
   // Note: This is could enhanced to also know about the ROOT ioctor
   // but this was not the case in CINT.
   if (!IsLoaded()) {
      return false;
   }
   const CXXRecordDecl *CRD =
      llvm::dyn_cast<CXXRecordDecl>(fDecl);
   if (!CRD) return true; 

   // For now make the object of non-public class, not createable ...
   // It would be better to find away to 'break' through
   // the privacy.
   if (CRD->getAccess() != AS_public && CRD->getAccess() != AS_none)
      return false;
   if (CRD->getName().equals("pair")) {
      // Very special case ... oh well ...
      const clang::ClassTemplateSpecializationDecl *tmplt_specialization = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl> (CRD);
      if (tmplt_specialization) {
         for(unsigned int i = 0; i <  tmplt_specialization->getTemplateArgs().size(); ++i) {
            const clang::TemplateArgument &arg( tmplt_specialization->getTemplateArgs().get(i) );
            clang::QualType tmplti = arg.getAsType();
            const clang::Type *type = ROOT::TMetaUtils::GetUnderlyingType(tmplti);
            if (type->isFundamentalType() || type->isEnumeralType()) {
               continue;
            }
            clang::RecordDecl *tdecl = type->getAsCXXRecordDecl();
            if (tdecl->getAccess() != AS_public && tdecl->getAccess() != AS_none)
               return false;
         }
      }
   }

   cling::Interpreter::PushTransactionRAII pushedT(fInterp);
   for (CXXRecordDecl::ctor_iterator iter = CRD->ctor_begin(),
         end = CRD->ctor_end(); iter != end; ++iter) {
      if (iter->getAccess() == AS_public) {
         // We can reach this constructor.
         if (iter->getNumParams() == 0) {
            return true;
         }
         // Most likely just this test is needed.
         if (iter->getMinRequiredArguments() == 0) {
            return true;
         }
      }
   }
   return false;
}

bool TClingClassInfo::HasMethod(const char *name) const
{
   if (IsLoaded() && !llvm::isa<EnumDecl>(fDecl)) {
      return fInterp->getLookupHelper().hasFunction(fDecl, name);
   }
   return false;
}

void TClingClassInfo::Init(const char *name)
{
   fFirstTime = true;
   fDescend = false;
   fIter = DeclContext::decl_iterator();
   fDecl = 0;
   fType = 0;
   fIterStack.clear();
   const cling::LookupHelper& lh = fInterp->getLookupHelper();
   fDecl = lh.findScope(name,&fType, /* intantiateTemplate= */ true );
   if (!fDecl) {
      std::string buf = TClassEdit::InsertStd(name);
      fDecl = lh.findScope(buf,&fType, /* intantiateTemplate= */ true );
   }
   if (!fDecl && fType) {
      const TagType *tagtype =fType->getAs<TagType>();
      if (tagtype) {
         fDecl = tagtype->getDecl();
      }
   }
}

void TClingClassInfo::Init(const Decl* decl)
{
   fFirstTime = true;
   fDescend = false;
   fIter = DeclContext::decl_iterator();
   fDecl = decl;
   fType = 0;
   fIterStack.clear();
}

void TClingClassInfo::Init(int tagnum)
{
   Fatal("TClingClassInfo::Init(tagnum)", "Should no longer be called");
   return;
}

void TClingClassInfo::Init(const Type &tag)
{
   fType = &tag;
   const TagType *tagtype = fType->getAs<TagType>();
   if (tagtype) {
      fDecl = tagtype->getDecl();
   }
   else {
      fDecl = 0;
   }
   if (!fDecl) {
      QualType qType(fType,0);
      static PrintingPolicy printPol(fInterp->getCI()->getLangOpts());
      printPol.SuppressScope = false;
      Error("TClingClassInfo::Init(const Type&)",
            "The given type %s does not point to a Decl",
            qType.getAsString(printPol).c_str());
   }
}

bool TClingClassInfo::IsBase(const char *name) const
{
   if (!IsLoaded()) {
      return false;
   }
   TClingClassInfo base(fInterp, name);
   if (!base.IsValid()) {
      return false;
   }
   const CXXRecordDecl *CRD =
      llvm::dyn_cast<CXXRecordDecl>(fDecl);
   if (!CRD) {
      // We are an enum, namespace, or translation unit,
      // we cannot be the base of anything.
      return false;
   }
   const CXXRecordDecl *baseCRD =
      llvm::dyn_cast<CXXRecordDecl>(base.GetDecl());
   return CRD->isDerivedFrom(baseCRD);
}

bool TClingClassInfo::IsEnum(cling::Interpreter *interp, const char *name)
{
   // Note: This is a static member function.
   TClingClassInfo info(interp, name);
   if (info.IsValid() && (info.Property() & kIsEnum)) {
      return true;
   }
   return false;
}

bool TClingClassInfo::IsLoaded() const
{
   // IsLoaded in CINT was meaning is known to the interpreter
   // and has a complete definition.
   // IsValid in Cling (as in CING) means 'just' is known to the 
   // interpreter.
   if (!IsValid()) {
      return false;
   }
   if (fDecl == 0) {
      return false;
   }
   const CXXRecordDecl *CRD = llvm::dyn_cast<CXXRecordDecl>(fDecl);
   if ( CRD ) {
      if (!CRD->hasDefinition()) {
         return false;
      }
   } else {
      const TagDecl *TD = llvm::dyn_cast<TagDecl>(fDecl);
      if (TD && TD->getDefinition() == 0) {
         return false;
      }
   }
   // All clang classes are considered loaded.
   return true;
}

bool TClingClassInfo::IsValid() const
{
   if(fType)
   {
      return fDecl && !fType->isIncompleteType(0);
   }
   else
   {
      return fDecl;
   }
}

bool TClingClassInfo::IsValidMethod(const char *method, const char *proto,
                                    Bool_t objectIsConst,
                                    long *offset,
                                    EFunctionMatchMode mode /*= kConversionMatch*/) const
{
   // Check if the method with the given prototype exist.
   if (!IsLoaded()) {
      return false;
   }
   if (offset) {
      *offset = 0L;
   }
   TClingMethodInfo mi = GetMethod(method, proto, offset, mode);
   return mi.IsValid();
}

int TClingClassInfo::InternalNext()
{
   if (!*fIter) {
      // Iterator is already invalid.
      if (fFirstTime && fDecl) {
         std::string buf;
         if (const NamedDecl* ND =
               llvm::dyn_cast<NamedDecl>(fDecl)) {
            PrintingPolicy Policy(fDecl->getASTContext().
               getPrintingPolicy());
            llvm::raw_string_ostream stream(buf);
            ND->getNameForDiagnostic(stream, Policy, /*Qualified=*/false);
         }
         Error("TClingClassInfo::InternalNext",
            "Next called but iteration not prepared for %s!", buf.c_str());
      }
      return 0;
   }
   cling::Interpreter::PushTransactionRAII pushedT(fInterp);
   while (true) {
      // Advance to next usable decl, or return if there is no next usable decl.
      if (fFirstTime) {
         // The cint semantics are strange.
         fFirstTime = false;
      }
      else {
         // Advance the iterator one decl, descending into the current decl
         // context if necessary.
         if (!fDescend) {
            // Do not need to scan the decl context of the current decl,
            // move on to the next decl.
            ++fIter;
         }
         else {
            // Descend into the decl context of the current decl.
            fDescend = false;
            //fprintf(stderr,
            //   "TClingClassInfo::InternalNext:  "
            //   "pushing ...\n");
            fIterStack.push_back(fIter);
            DeclContext *DC = llvm::cast<DeclContext>(*fIter);
            fIter = DC->decls_begin();
         }
         // Fix it if we went past the end.
         while (!*fIter && fIterStack.size()) {
            //fprintf(stderr,
            //   "TClingClassInfo::InternalNext:  "
            //   "popping ...\n");
            fIter = fIterStack.back();
            fIterStack.pop_back();
            ++fIter;
         }
         // Check for final termination.
         if (!*fIter) {
            // We have reached the end of the translation unit, all done.
            fDecl = 0;
            fType = 0;
            return 0;
         }
      }
      // Return if this decl is a class, struct, union, enum, or namespace.
      Decl::Kind DK = fIter->getKind();
      if ((DK == Decl::Namespace) || (DK == Decl::Enum) ||
            (DK == Decl::CXXRecord) ||
            (DK == Decl::ClassTemplateSpecialization)) {
         const TagDecl *TD = llvm::dyn_cast<TagDecl>(*fIter);
         if (TD && !TD->isCompleteDefinition()) {
            // For classes and enums, stop only on definitions.
            continue;
         }
         if (DK == Decl::Namespace) {
            // For namespaces, stop only on the first definition.
            if (!fIter->isCanonicalDecl()) {
               // Not the first definition.
               fDescend = true;
               continue;
            }
         }
         if (DK != Decl::Enum) {
            // We do not descend into enums.
            DeclContext *DC = llvm::cast<DeclContext>(*fIter);
            if (*DC->decls_begin()) {
               // Next iteration will begin scanning the decl context
               // contained by this decl.
               fDescend = true;
            }
         }
         // Iterator is now valid.
         fDecl = *fIter;
         fType = 0;
         if (fDecl) {
            if (const RecordDecl *RD =
                  llvm::dyn_cast<RecordDecl>(fDecl)) {
               fType = RD->getASTContext().getRecordType(RD).getTypePtr();
            }
         }
         return 1;
      }
   }
}

int TClingClassInfo::Next()
{
   return InternalNext();
}
   
void *TClingClassInfo::New() const
{
   // Invoke a new expression to use the class constructor
   // that takes no arguments to create an object of this class type.

   if (!HasDefaultConstructor()) {
      return 0;
   }
   std::ostringstream os;
   os << "new " << FullyQualifiedName(fDecl) << ";";
   cling::StoredValueRef val;
   cling::Interpreter::CompilationResult err =
      fInterp->evaluate(os.str(), val);
   if (err != cling::Interpreter::kSuccess) {
      return 0;
   }
   // The ref-counted pointer will get destructed by StoredValueRef,
   // but not the allocation! I.e. the following is fine:
   return llvm::GVTOP(val.get().getGV());
}

void *TClingClassInfo::New(int n) const
{
   // Invoke a new expression to use the class constructor
   // that takes no arguments to create an array object
   // of this class type.
   if (!HasDefaultConstructor()) {
      return 0;
   }
   std::ostringstream os;
   os << "new " << FullyQualifiedName(fDecl) << "[" << n << "];";
   cling::StoredValueRef val;
   cling::Interpreter::CompilationResult err =
      fInterp->evaluate(os.str(), val);
   if (err != cling::Interpreter::kSuccess) {
      return 0;
   }
   // The ref-counted pointer will get destructed by StoredValueRef,
   // but not the allocation! I.e. the following is fine:
   return llvm::GVTOP(val.get().getGV());
}

void *TClingClassInfo::New(int n, void *arena) const
{
   // Invoke a placement new expression to use the class
   // constructor that takes no arguments to create an
   // array of objects of this class type in the given
   // memory arena.
   if (!HasDefaultConstructor()) {
      return 0;
   }
   std::ostringstream os;
   os << "new ((void*)" << reinterpret_cast<unsigned long>(arena) << ") "
      << FullyQualifiedName(fDecl) << "[" << n << "];";
   cling::StoredValueRef val;
   cling::Interpreter::CompilationResult err =
      fInterp->evaluate(os.str(), val);
   if (err != cling::Interpreter::kSuccess) {
      return 0;
   }
   // The ref-counted pointer will get destructed by StoredValueRef,
   // but not the allocation! I.e. the following is fine:
   return llvm::GVTOP(val.get().getGV());
}

void *TClingClassInfo::New(void *arena) const
{
   // Invoke a placement new expression to use the class
   // constructor that takes no arguments to create an
   // object of this class type in the given memory arena.
   if (!HasDefaultConstructor()) {
      return 0;
   }
   std::ostringstream os;
   os << "new ((void*)" << reinterpret_cast<unsigned long>(arena) << ") "
      << FullyQualifiedName(fDecl) << ";";
   cling::StoredValueRef val;
   cling::Interpreter::CompilationResult err =
      fInterp->evaluate(os.str(), val);
   if (err != cling::Interpreter::kSuccess) {
      return 0;
   }
   // The ref-counted pointer will get destructed by StoredValueRef,
   // but not the allocation! I.e. the following is fine:
   return llvm::GVTOP(val.get().getGV());
}

long TClingClassInfo::Property() const
{
   if (!IsValid()) {
      return 0L;
   }
   long property = 0L;
   property |= kIsCPPCompiled;
   if (fDecl->getDeclContext()->Equals(fInterp->getSema().getStdNamespace())) {
      property |= kIsDefinedInStd;
   }
   Decl::Kind DK = fDecl->getKind();
   if ((DK == Decl::Namespace) || (DK == Decl::TranslationUnit)) {
      property |= kIsNamespace;
      return property;
   }
   // Note: Now we have class, enum, struct, union only.
   const TagDecl *TD = llvm::dyn_cast<TagDecl>(fDecl);
   if (!TD) {
      return 0L;
   }
   if (TD->isEnum()) {
      property |= kIsEnum;
      return property;
   }
   // Note: Now we have class, struct, union only.
   const CXXRecordDecl *CRD =
      llvm::dyn_cast<CXXRecordDecl>(fDecl);
   if (CRD->isClass()) {
      property |= kIsClass;
   }
   else if (CRD->isStruct()) {
      property |= kIsStruct;
   }
   else if (CRD->isUnion()) {
      property |= kIsUnion;
   }
   if (CRD->hasDefinition() && CRD->isAbstract()) {
      property |= kIsAbstract;
   }
   return property;
}

int TClingClassInfo::RootFlag() const
{
   if (!IsValid()) {
      return 0;
   }
   // FIXME: Implement this when rootcling provides the value.
   return 0;
}

int TClingClassInfo::Size() const
{
   if (!IsValid()) {
      return -1;
   }
   if (!fDecl) {
      // A forward declared class.
      return 0;
   }
   Decl::Kind DK = fDecl->getKind();
   if (DK == Decl::Namespace) {
      // Namespaces are special for cint.
      return 1;
   }
   else if (DK == Decl::Enum) {
      // Enums are special for cint.
      return 0;
   }
   const RecordDecl *RD = llvm::dyn_cast<RecordDecl>(fDecl);
   if (!RD) {
      // Should not happen.
      return -1;
   }
   if (!RD->getDefinition()) {
      // Forward-declared class.
      return 0;
   }
   ASTContext &Context = fDecl->getASTContext();
   const ASTRecordLayout &Layout = Context.getASTRecordLayout(RD);
   int64_t size = Layout.getSize().getQuantity();
   int clang_size = static_cast<int>(size);
   return clang_size;
}

long TClingClassInfo::Tagnum() const
{
   if (!IsValid()) {
      return -1L;
   }
   return reinterpret_cast<long>(fDecl);
}

const char *TClingClassInfo::FileName()
{
   if (!IsValid()) {
      return 0;
   }
   fDeclFileName = ROOT::TMetaUtils::GetFileName(GetDecl());
   return fDeclFileName.c_str();
}

const char *TClingClassInfo::FullName(const ROOT::TMetaUtils::TNormalizedCtxt &normCtxt) const
{
   // Return QualifiedName.
   if (!IsValid()) {
      return 0;
   }
   // Note: This *must* be static because we are returning a pointer inside it!
   static std::string buf;
   buf.clear();
   if (fType) {
      QualType type(fType, 0);
      ROOT::TMetaUtils::GetNormalizedName(buf, type, *fInterp, normCtxt);
   }
   else {
      if (const NamedDecl* ND =
            llvm::dyn_cast<NamedDecl>(fDecl)) {
         PrintingPolicy Policy(fDecl->getASTContext().
            getPrintingPolicy());
         llvm::raw_string_ostream stream(buf);
         ND->getNameForDiagnostic(stream, Policy, /*Qualified=*/true);
      }
   }
   return buf.c_str();
}

const char *TClingClassInfo::Name() const
{
   // Return unqualified name.
   if (!IsValid()) {
      return 0;
   }
   // Note: This *must* be static because we are returning a pointer inside it!
   static std::string buf;
   buf.clear();
   if (const NamedDecl* ND = llvm::dyn_cast<NamedDecl>(fDecl)) {
      PrintingPolicy Policy(fDecl->getASTContext().getPrintingPolicy());
      llvm::raw_string_ostream stream(buf);
      ND->getNameForDiagnostic(stream, Policy, /*Qualified=*/false);
   }
   return buf.c_str();
}

const char *TClingClassInfo::Title()
{
   if (!IsValid()) {
      return 0;
   }
   // NOTE: We cannot cache the result, since we are really an iterator.
   // Try to get the comment either from the annotation or the header
   // file, if present.
   // Iterate over the redeclarations, we can have muliple definitions in the
   // redecl chain (came from merging of pcms).
   if (const TagDecl *TD = llvm::dyn_cast<TagDecl>(GetDecl())) {
      if ( (TD = ROOT::TMetaUtils::GetAnnotatedRedeclarable(TD)) ) {
         if (AnnotateAttr *A = TD->getAttr<AnnotateAttr>()) {
            fTitle = A->getAnnotation().str();
            return fTitle.c_str();
         }
      }
   }
   // Try to get the comment from the header file, if present.
   const CXXRecordDecl *CRD =
      llvm::dyn_cast<CXXRecordDecl>(GetDecl());
   if (CRD) {
      fTitle = ROOT::TMetaUtils::GetClassComment(*CRD,0,*fInterp).str();
   }
   return fTitle.c_str();
}

const char *TClingClassInfo::TmpltName() const
{
   if (!IsValid()) {
      return 0;
   }
   // Note: This *must* be static because we are returning a pointer inside it!
   static std::string buf;
   buf.clear();
   if (const NamedDecl* ND = llvm::dyn_cast<NamedDecl>(fDecl)) {
      // Note: This does *not* include the template arguments!
      buf = ND->getNameAsString();
   }
   return buf.c_str();
}

