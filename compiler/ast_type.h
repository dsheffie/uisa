/* File: ast_type.h
 *
 * In our parse tree, Type nodes are used to represent and store type
 * information. The base Type class is used for built-in types, the NamedType
 * for classes and interfaces, and the ArrayType for arrays of other types.
 *
 * pp3: You will need to extend the Type classes to implement
 * the type system and rules for type equivalency and compatibility.
 */

#ifndef AST_TYPE_H
#define AST_TYPE_H

#include "ast.h"
#include "list.h"
#include <iostream>
using namespace std;

class Type : public Node
{
  protected:
    char *typeName;

  public :
    /*a special type is added to types, to handle void* produced by malloc*/
    static Type *intType, *doubleType, *boolType, *voidType,
      *nullType, *stringType, *errorType, *specialType;

    Type(yyltype loc) : Node(loc) {}
    Type(const char *str);
    
    /* Overload the == and != methods for type checking.
       Easier to use / understand than the IsEquivalentTo()
       method
    */
    virtual bool operator==(Type &rhs) const;
    virtual bool operator!=(Type &rhs) const;
    
    const char *getPrintNameForNode() { return "Type"; }
    void printChildren(int indentLevel);

    //print type string
    virtual char* getType();

    //dsheffie:
    bool checkChildren(Check *c);
    virtual bool isArray() { return false; }
    virtual bool isNamed() { return false; }
    bool isNull();

    virtual void PrintToStream(ostream& out) { out << typeName; }
    friend ostream& operator<<(ostream& out, Type *t) 
    { t->PrintToStream(out); return out; }

    //Comparison, but not using == or != operators
    virtual bool IsEquivalentTo(Type *other) {  return this == other; }
    static bool CompareTypes(Check * c, Type* t0, Type* t1);
};

class NamedType : public Type
{
  protected:
    Identifier *id;
    bool isClassType;
    bool isInterfaceType;

  public:
    NamedType(Identifier *i);

    const char *getPrintNameForNode() { return "NamedType"; }
    void printChildren(int indentLevel);

    char* getType();
    Identifier* getId() { return id; }
    
    //Override virtual methods
    bool operator==(NamedType &rhs) const;
    bool operator!=(NamedType &rhs) const;
    bool checkChildren(Check *c);
    
    //Quick methods to determine type
    bool isNamed() { return true; }
    bool isClass(){ return isClassType;}
    bool isInterface() { return isInterfaceType;}

    void PrintToStream(ostream& out) { out << id; }
};

class ArrayType : public Type
{
  protected:
    Type *elemType;

  public:
    ArrayType(yyltype loc, Type *elemType);
 
    const char *getPrintNameForNode() { return "ArrayType"; }
    void printChildren(int indentLevel);

    //Override virtual methods
    bool operator==(ArrayType &rhs) const;
    bool operator!=(ArrayType &rhs) const;
    bool checkChildren(Check *c);
    bool isArray() { return true; }

    void PrintToStream(ostream& out) { out << elemType << "[]"; }
    bool IsEquivalentTo(Type *other);
    Type *getElemType() { return elemType; }
    char* getType();
 
};

#endif /* AST_TYPE_H */
