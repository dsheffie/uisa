/* File: ast_decl.h
 *
 * Decl nodes are used to represent and manage declarations. There are 4
 * subclasses of the base class, specialized for declarations of variables,
 * functions, classes, and interfaces.
 *
 * pp3: You will need to extend the Decl classes to implement
 * semantic processing including detection of declaration conflicts
 * and managing scoping issues.
 */

#ifndef AST_DECL_H
#define AST_DECL_H

#include <cstdio>

#include "ast.h"
#include "list.h"
#include "ast_type.h"
#include "errors.h"
#include "hashtable.h"

#include "llvmInc.hh"


class Type;
class NamedType;
class Identifier;
class Stmt;
class Scope;
class Expr;


class Decl : public Node
{
  protected:
    Identifier *id;

  public: 

    Decl(Identifier *name);
 
    /* Virtual functions used to dynamically 
     * dispatch different types checks */
 
    virtual char* getName();
    virtual bool isArray() { return false; }
 
    virtual char* getType() 
    { 
      return "Generic Decl"; 
    }
    virtual Type* getVarType() { return Type::errorType; }
    virtual bool isFnDecl() { return false;}
    virtual bool isVarDecl(){ return false;}
    virtual Identifier* getID(){return id;}
    virtual void genVTable() {}
 
    //Used by interface / fn decls, class overrides
    virtual void getSignature(Check *c) { 
      if(!c->addDecl(this)){
	  ReportError::DeclConflict(this, c->getDecl(this->getName()));
	  return;
      } }
    friend ostream& operator<<(ostream& out, Decl *d) { return out << d->id; }
   

};

class VarDecl : public Decl
{
  protected:
    Type *type;
    bool isClass;
    ClassDecl* myClass;
  public:
    bool operator==(const VarDecl &rhs) const;
    bool operator!=(const VarDecl &rhs) const;

    VarDecl(Identifier *name, Type *type);
    const char *getPrintNameForNode() { return "VarDecl"; }
    void printChildren(int indentLevel);

    //Returns true if the variable is contained in a classs
    bool checkClass();
    //Returns class reference if containted in a class
    ClassDecl* getClass() { return myClass; }
    //Set class reference
    void setClass(ClassDecl* myClass);
    
    //Check that the variable reference is valid
    bool checkChildren(Check *c);
    //Used for dynamic dispatch
    bool isVarDecl() { return true; }
    //Return variable type
    Type* getVarType() { return type; }
   
    bool isArray() { return type->isArray(); }
    char* getType() { return type->getType(); }

    void emit();
};

class InterfaceDecl : public Decl
{
  protected:
    List<Decl*> *members;
    List<FnDecl*> *interface_decls;
  public:
    InterfaceDecl(Identifier *name, List<Decl*> *members);
    const char *getPrintNameForNode() { return "InterfaceDecl"; }
    void printChildren(int indentLevel);
    
    //Return reference if method with "name" exists
    FnDecl* findMethod(char* name);
    
    /* Given a list of methods, checkMethods()
     * will return true if all the methods declared
     * in the interface are found in the list */
    bool checkMethods(List<FnDecl*> *fd, Check *c);
    
    /* Add a method signature to the interface */
    void addPrototype(FnDecl* fd);
    
    /* Semantic checking method */
    bool checkChildren(Check *c);
 
    /* Return a declaration list */
    List<Decl*>* getMembers();
    char* getType() { return (char*)getPrintNameForNode(); }
    bool isInterfaceDecl() { return true; }

    void emit()
    { printf("Interface not supported\n"); exit(-1); }
};

class FnDecl : public Decl
{
  protected:
    List<VarDecl*> *formals;
    Type *returnType;
    Stmt *body;
    //pp4
    Scope * myScope;
    
    //namespace for pp4
    char* CodeLabel;

 public:
    llvm::FunctionType *getFuncSignature(llvm::LLVMContext *Context,
					 std::set<ClassDecl*> &classes,
					 std::vector<std::string> &argNames);
    FnDecl(Identifier *name, Type *returnType, List<VarDecl*> *formals);
    void setFunctionBody(Stmt *b);
    const char *getPrintNameForNode() { return "FnDecl"; }
    void printChildren(int indentLevel);

    /* Check that function parameters match signature */
    bool validParameters(List<Expr*> *p, Identifier* where,Check* c);

    bool checkChildren(Check *c);
    char* getType() { return (char*)getPrintNameForNode(); }
    bool isFnDecl() { return true; }

    bool operator==(const FnDecl &rhs) const;
    bool operator!=(const FnDecl &rhs) const;
    
    Type* getReturn() { return returnType; }

    char* getLabel() { return CodeLabel; }

    void emit();
};


class ClassDecl : public Decl
{
  protected:
    List<Decl*> *members;
    NamedType *extends;
    List<NamedType*> *implements;
    List<FnDecl*> *methods;
    List<VarDecl*> *member_decls;

    /* List of interface references that the
     * class needs to implement */
    List<InterfaceDecl*> *interfaces_to_implement;
    //pp4
    Scope * myScope;
    Scope* class_scope;

    //vptr, variable a, variable b,etc..
    Hashtable <int> *ClassVarOffsets;
    
    //In the VTable
    Hashtable <int> *MethodOffsets;
    int currentOffset;

  public:
    ClassDecl(Identifier *name, NamedType *extends,
              List<NamedType*> *implements, List<Decl*> *members);
    const char *getPrintNameForNode() { return "ClassDecl"; }

    void extend_class(ClassDecl* base,Check *c);
    void implement_class(InterfaceDecl* base);

    void printChildren(int indentLevel);

    void getSignature(Check *c);

    void makeVTable(llvm::LLVMContext *Context,
		    llvm::Module *myModule,
		    llvm::IRBuilder<> *myIRBuilder,
		    std::map<std::string, llvm::Value*> &labelMap,
		    std::map<std::string, std::vector<std::string> > &vtableNames);
    
    //dsheffie:
    bool checkChildren(Check *c);
    char* getType() { return (char*)getPrintNameForNode(); }
    bool isClassDecl() { return true; }
    void addToClassScope(Decl* d);

    char* getName() { return id->getName(); }

    //void addMethod(FnDecl *fd) { methods->append(fd); }
    //void addMember(VarDecl *vd) { member_decls->append(vd); }

    Decl* findInClass(char *name);    
    FnDecl* findMethod(char *name);
    VarDecl* findMember(char *name);
    Identifier* getId(){return id;};
    NamedType* getBaseClass() { return extends; }
    List<NamedType*>* getImplementClass() {return implements;}
    Scope* getScope();
    /* Return a declaration list */
    List<Decl*>* getMembers() {return members;}

    void emit();
    void genVTable();
    //todo...change methodName to memberName
    void addVariableOffset(char* memberName);
    int findVariableOffset(char *memberName);
    int findMethodOffset(char *methodName);
    Hashtable <int> *getClassVarOffsets(){return ClassVarOffsets;}
    int getVariableOffset();
 private:
};


#endif /* AST_DECL_H */
