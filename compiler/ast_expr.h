// -*-c++-*-
/* File: ast_expr.h
 *
 * The Expr class and its subclasses are used to represent expressions in the
 * parse tree.  For each expression in the language (add, call, New, etc.) there
 * is a corresponding node class for that construct.
 *
 * pp3: You will need to extend the Expr classes to implement
 * semantic analysis for rules pertaining to expressions.
 */

#ifndef AST_EXPR_H
#define AST_EXPR_H

#include "ast.h"
#include "ast_stmt.h"
#include "ast_type.h"
#include "list.h"
#include "tac.h"

class NamedType;
class Type;

class Expr : public Stmt {
 public:
  Expr(yyltype loc) : Stmt(loc) {}
  Expr() : Stmt() {}
  virtual bool isConstant() {
    return false;
  }
  virtual Type* myType(Check *c) {
    return ExprType;
  }
  Type* myType() {
    return ExprType;
  }
  virtual char* getName() {
    return nullptr;
  }
  Type* ExprType;
};

/* This node type is used for those places where an expression is optional.
 * We could use a NULL pointer, but then it adds a lot of checking for
 * NULL. By using a valid, but no-op, node, we save that trouble */
class EmptyExpr : public Expr
{
  public:
  EmptyExpr();
    const char *getPrintNameForNode() { return "Empty"; }
};

class IntConstant : public Expr
{
  protected:
    int value;

  public:
    IntConstant(yyltype loc, int val);
    const char *getPrintNameForNode() { return "IntConstant"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    bool isConstant() { return true; }
    Type* myType(Check *c) { return Type::intType; }
    //for pp4
    void emit();
};

class DoubleConstant : public Expr
{
  protected:
    double value;

  public:
    DoubleConstant(yyltype loc, double val);
    const char *getPrintNameForNode() { return "DoubleConstant"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    bool isConstant() { return true; }
    Type* myType(Check *c) { return Type::doubleType; }
    void emit();
};

class BoolConstant : public Expr
{
  protected:
    bool value;

  public:
    BoolConstant(yyltype loc, bool val);
    const char *getPrintNameForNode() { return "BoolConstant"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    bool isConstant() { return true; }
    Type* myType(Check *c) { return Type::boolType; }

    //for pp4
    void emit();
};

class StringConstant : public Expr
{
  protected:
    char *value;

  public:
    StringConstant(yyltype loc, const char *val);
    void emit();

    const char *getPrintNameForNode() { return "StringConstant"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    bool isConstant() { return true; }
    Type* myType(Check *c) { return Type::stringType; }
};

class NullConstant: public Expr
{
  public:
    NullConstant(yyltype loc) : Expr(loc) {}
    const char *getPrintNameForNode() { return "NullConstant"; }
    bool isConstant() { return true; }
    bool checkChildren(Check *c);
    void emit();
};

class Operator : public Node
{
  protected:
    char tokenString[4];

  public:
    Operator(yyltype loc, const char *tok);
    const char *getOpString(){return tokenString;}
    const char *getPrintNameForNode() { return "Operator"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    friend ostream& operator<<(ostream& out, Operator *o) 
    { return out << o->tokenString; }
 };

class CompoundExpr : public Expr
{
  protected:
    Operator *op;
    Expr *left, *right; // left will be NULL if unary

  public:
    CompoundExpr(Expr *lhs, Operator *op, Expr *rhs); // for binary
    CompoundExpr(Operator *op, Expr *rhs);             // for unary
    CompoundExpr(Expr *lhs, Operator *op);             // for unary
    void printChildren(int indentLevel);

    bool checkChildren(Check *c);

};

class ArithmeticExpr : public CompoundExpr
{
  public:
    ArithmeticExpr(Expr *lhs, Operator *op, Expr *rhs) : CompoundExpr(lhs,op,rhs) {}
    ArithmeticExpr(Operator *op, Expr *rhs) : CompoundExpr(op,rhs) {}
    ArithmeticExpr(Expr *lhs, Operator *op) : CompoundExpr(lhs,op) {}
    const char *getPrintNameForNode() { return "ArithmeticExpr"; }
    bool checkChildren(Check *c);
    void emit();
};

class RelationalExpr : public CompoundExpr
{
 public:
 RelationalExpr(Expr *lhs, Operator *op, Expr *rhs) : 
  CompoundExpr(lhs,op,rhs) {}
  
  bool checkChildren(Check *c);
  const char *getPrintNameForNode() { return "RelationalExpr"; }
  void emit();
};

class EqualityExpr : public CompoundExpr
{
  public:
    EqualityExpr(Expr *lhs, Operator *op, Expr *rhs) : 
  CompoundExpr(lhs,op,rhs) {}
  bool checkChildren(Check *c);
  void emit();

  const char *getPrintNameForNode() { return "EqualityExpr"; }
};

class LogicalExpr : public CompoundExpr
{
 public:
 LogicalExpr(Expr *lhs, Operator *op, Expr *rhs) : 
  CompoundExpr(lhs,op,rhs) {} 
  LogicalExpr(Operator *op, Expr *rhs) : CompoundExpr(op,rhs) {}
  const char *getPrintNameForNode() { return "LogicalExpr"; }
  bool checkChildren(Check *c);
  void emit();
};

class AssignExpr : public CompoundExpr
{
  public:
  bool checkChildren(Check *c);

 AssignExpr(Expr *lhs, Operator *op, Expr *rhs) : CompoundExpr(lhs,op,rhs) {}
  const char *getPrintNameForNode() { return "AssignExpr"; }
  void emit ();
};

class LValue : public Expr
{
  public:
    LValue(yyltype loc) : Expr(loc) {}
};

class This : public Expr
{
 private:
  ClassDecl *myClass;
  public:
    This(yyltype loc) : Expr(loc) {}
    const char *getPrintNameForNode() { return "This"; }
    bool checkChildren(Check *c);
    void emit();
};

class ArrayAccess : public LValue
{
  protected:
    Expr *base, *subscript;

  public:
    ArrayAccess(yyltype loc, Expr *base, Expr *subscript);
    const char *getPrintNameForNode() { return "ArrayAccess"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    void emit();
    void emit_addr();
 private:
    void emitDriver();
};

/* Note that field access is used both for qualified names base.field and just
 * field without qualification. We don't know for sure whether there is an
 * implicit "this." in front until later on, so we use one node type for either
 * and sort it out later. */
class FieldAccess : public LValue
{
  protected:
    Expr *base;	// will be NULL if no explicit base
    Identifier *field;
    
    //if we're accessing a class member
    ClassDecl* myClass;
  public:
    FieldAccess(Expr *base, Identifier *field); //ok to pass NULL base
    const char *getPrintNameForNode() { return "FieldAccess"; }
    void printChildren(int indentLevel); 
    bool checkChildren(Check *c);
    Type* myType(Check *c);
    char* getName() { return field->getName(); }
    bool isMember(){return (myClass!=NULL);}
    void emit();
    void emit_addr();
    bool hasThis();
    bool hasBase();
    bool isClass();
    bool isPointer();

};

/* Like field access, call is used both for qualified base.field() and
 * unqualified field().  We won't figure out until later whether we need
 * implicit "this." so we use one node type for either and sort it out later. */
class Call : public Expr
{
  protected:
    Expr *base;	// will be NULL if no explicit base
    Identifier *field;
    List<Expr*> *actuals;

  public:
    Call(yyltype loc, Expr *base, Identifier *field, List<Expr*> *args);
    const char *getPrintNameForNode() { return "Call"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    
    void emit();
};

/*used to specically handle the setByte library call*/
class SetByte : public Expr
{
  protected:
    Expr *base;
    Expr * e1;
    Expr * e2;

  public:
    SetByte(yyltype loc, Expr *base,  Expr* e1, Expr* e2);
    const char *getPrintNameForNode() { return "SetByte"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
};
/*used to specically handle the getByte library call*/
class GetByte : public Expr
{
  protected:
    Expr *base;
    Expr * e1;

  public:
    GetByte(yyltype loc, Expr *base,  Expr* e1);
    const char *getPrintNameForNode() { return "GetByte"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
};


class NewExpr : public Expr
{
  protected:
    NamedType *cType;

  public:
    NewExpr(yyltype loc, NamedType *clsType);
    const char *getPrintNameForNode() { return "NewExpr"; }
    void printChildren(int indentLevel);
    //dsheffie:
    bool checkChildren(Check *c);
    void emit();
};

class NewArrayExpr : public Expr
{
  protected:
    Expr *size;
    Type *elemType;

  public:
    NewArrayExpr(yyltype loc, Expr *sizeExpr, Type *elemType);
    const char *getPrintNameForNode() { return "NewArrayExpr"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    void emit();
};

class MallocExpr : public Expr
{
  protected:
    Expr *size;

  public:
    MallocExpr(yyltype loc, Expr *sizeExpr);
    const char *getPrintNameForNode() { return "MallocExpr"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    void emit();
};

class ReadIntegerExpr : public Expr
{
  public:
    ReadIntegerExpr(yyltype loc) : Expr(loc) {}
    const char *getPrintNameForNode() { return "ReadIntegerExpr"; }
    bool checkChildren(Check *c);
    void emit();
};

class ReadLineExpr : public Expr
{
  public:
    ReadLineExpr(yyltype loc) : Expr (loc) {}
    const char *getPrintNameForNode() { return "ReadLineExpr"; }
    bool checkChildren(Check *c);
    void emit();
};

#endif /* AST_EXPR_H */
