/* File: ast_stmt.h
 *
 * The Stmt class and its subclasses are used to represent statements in the
 * parse tree.  For each statment in the language (for, if, return, etc.) there
 * is a corresponding node class for that construct.
 *
 * pp3: You will need to extend the Stmt classes to implement
 * semantic analysis for rules pertaining to statements.
 */

#ifndef AST_STMT_H
#define AST_STMT_H

#include "list.h"
#include "ast.h"

class Decl;
class VarDecl;
class Expr;

class Program : public Node {
 protected:
  List<Decl*> *decls;
  bool main;
 public:
  Program(List<Decl*> *declList);
  
  //for PP4
  void emitprog(std::string &srcName);
  
  const char *getPrintNameForNode() { return "Program"; }
  void printChildren(int indentLevel);
  bool check();
  bool check(Check *c);
};

class Stmt : public Node {
 public:
 Stmt() : Node() {}
 Stmt(yyltype loc) : Node(loc) {}
};

class StmtBlock : public Stmt
{
  protected:
    List<VarDecl*> *decls;
    List<Stmt*> *stmts;
    Scope* myScope;
  public:
    StmtBlock(List<VarDecl*> *variableDeclarations, List<Stmt*> *statements);
    const char *getPrintNameForNode() { return "StmtBlock"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    //for PP4
    void emit();
};

class ConditionalStmt : public Stmt
{
  protected:
    Expr *test;
    Stmt *body;
  public:
    bool validTest(Check *c);
    ConditionalStmt(Expr *testExpr, Stmt *body);
    //for PP4
    void emit();
};

class LoopStmt : public ConditionalStmt
{
  public:
    LoopStmt(Expr *testExpr, Stmt *body)
            : ConditionalStmt(testExpr, body) {}
};

class ForStmt : public LoopStmt
{
  protected:
    Expr *init, *step;

  public:
    void emit();
    ForStmt(Expr *init, Expr *test, Expr *step, Stmt *body);
    const char *getPrintNameForNode() { return "ForStmt"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);

};

class WhileStmt : public LoopStmt
{
  public:
    WhileStmt(Expr *test, Stmt *body) : LoopStmt(test, body) {}
    const char *getPrintNameForNode() { return "WhileStmt"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    //for PP4
    void emit();

};

class IfStmt : public ConditionalStmt
{
  protected:
    Stmt *elseBody;

  public:
    IfStmt(Expr *test, Stmt *thenBody, Stmt *elseBody);
    const char *getPrintNameForNode() { return "IfStmt"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    void emit();
};

class BreakStmt : public Stmt
{
  public:
    BreakStmt(yyltype loc) : Stmt(loc) {}
    const char *getPrintNameForNode() { return "BreakStmt"; }
    bool checkChildren(Check *c);
    //for PP4
    void emit();

};

class ReturnStmt : public Stmt
{
  protected:
    Expr *expr;

  public:
    ReturnStmt(yyltype loc, Expr *expr);
    const char *getPrintNameForNode() { return "ReturnStmt"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    //for PP4
    void emit();
};


class PrintStmt : public Stmt
{
  protected:
    List<Expr*> *args;

  public:
    PrintStmt(List<Expr*> *arguments);
    const char *getPrintNameForNode() { return "PrintStmt"; }
    void printChildren(int indentLevel);
    bool checkChildren(Check *c);
    //for PP4
    void emit();
    
};


class CaseStmt : public Stmt
{
 protected:
  Expr  *caseint;
  List<Stmt*> *stmt;
 public:
  CaseStmt(Expr  *ci,List<Stmt*> *statement);
  const char *getPrintNameForNode(){return "CaseStmt";}
  void printChildren(int indentLevel);
  void emit();
  Expr* getTest (){return caseint;}
  bool checkChildren(Check *c);
};

class SwitchStmt : public Stmt
{
  protected:
  Expr * test;
     List<CaseStmt*> *casebody;
     List<Stmt*> *defaultbody;
     List<Expr*> *tests;
  public:
     SwitchStmt(Expr *t, List<CaseStmt*> *cb, List<Stmt*> *def);
     const char *getPrintNameForNode() { return "SwitchStmt"; }
     void printChildren(int indentLevel);
     void emit();
     bool checkChildren(Check *c);
};

#endif /* AST_STMT_H */
