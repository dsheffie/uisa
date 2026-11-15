// -*-c++-*-
/* ast.h: the abstract syntax tree of the millicode C subset.
 *
 * Forked from Decaf's ast*.h. The node hierarchy (Node / Expr / Stmt /
 * declarations) and the "each node emits its own TAC" structure are Decaf's;
 * the Java object model is gone. Semantic checks happen during TAC
 * generation (ast.cpp), which also applies C's integer promotions and
 * conversions so that the TAC is plain 32-bit arithmetic. 64-bit values are
 * pairs of 32-bit words from here on: the back end never sees them.
 */
#ifndef UCC_AST_H
#define UCC_AST_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "tac.h"

extern int yylineno;

struct StructDef;

struct Type {
  int bits = 32;
  bool sign = true;
  bool isVoid = false;
  StructDef *sd = nullptr;    // struct types: the definition
  Type() {}
  Type(int bits, bool sign) : bits(bits), sign(sign) {}
  static Type voidType() { Type t; t.isVoid = true; return t; }
  static Type s32() { return Type(32, true); }
  static Type u32() { return Type(32, false); }
  bool is64() const { return !isVoid && !sd && bits == 64; }
  bool isStruct() const { return sd != nullptr; }
  int words() const;          // 32-bit words a value of this type occupies
  std::string str() const;
  bool operator==(const Type &o) const {
    return isVoid == o.isVoid && sd == o.sd && (sd || (bits == o.bits && sign == o.sign));
  }
};

// struct name { fields }: plain value semantics, scalar or struct fields.
struct StructDef {
  std::string name;
  std::vector<std::pair<std::string, Type>> fields;
  int find(const std::string &f) const {
    for (size_t i = 0; i < fields.size(); i++)
      if (fields[i].first == f) return (int)i;
    return -1;
  }
};

// Struct and typedef names; the scanner returns them as type tokens (the
// classic C lexer hack).
extern std::map<std::string, Type> g_typeNames;

class CG;
class Block;

// The value of an expression: a TAC operand plus its C type. Every value is
// kept canonical for its type (u8 zero-extended, s8 sign-extended, ...), so
// integer promotion never needs an instruction. A 64-bit value is two words:
// `op` holds the low 32 bits and `hi` the high 32 bits. A struct value is
// its fields.
struct EV {
  Operand op;
  Type ty;
  Operand hi;
  std::vector<EV> fields;
  bool isConst() const { return !ty.isStruct() && op.isImm() && (!ty.is64() || hi.isImm()); }
};

struct Sym;

class Node {
 public:
  int line;
  Node() : line(yylineno) {}
  virtual ~Node() {}
};

class Expr : public Node {
 public:
  virtual EV gen(CG &cg) = 0;
  // Branch to t if the expression is nonzero, else to f.
  virtual void genCond(CG &cg, BasicBlock *t, BasicBlock *f);
  // The variable (or struct field) an assignment writes, or null.
  virtual Sym *lvalueSym(CG &cg) { (void)cg; return nullptr; }
};

class IntLit : public Expr {
 public:
  uint64_t value; Type ty;
  IntLit(uint64_t v, Type t) : value(v), ty(t) {}
  EV gen(CG &cg) override;
};

class VarRef : public Expr {
 public:
  std::string name;
  VarRef(const char *n) : name(n) {}
  EV gen(CG &cg) override;
  Sym *lvalueSym(CG &cg) override;
};

class MemberExpr : public Expr {   // base.field
 public:
  Expr *base; std::string field;
  MemberExpr(Expr *b, const char *f) : base(b), field(f) {}
  EV gen(CG &cg) override;
  Sym *lvalueSym(CG &cg) override;
};

class UnaryExprNode : public Expr {
 public:
  int op; Expr *e;
  UnaryExprNode(int op, Expr *e) : op(op), e(e) {}
  EV gen(CG &cg) override;
  void genCond(CG &cg, BasicBlock *t, BasicBlock *f) override;
};

class BinaryExpr : public Expr {
 public:
  int op; Expr *l, *r;      // op: + - * / % & | ^ L(<<) R(>>)
  BinaryExpr(int op, Expr *l, Expr *r) : op(op), l(l), r(r) {}
  EV gen(CG &cg) override;
};

class CompareExpr : public Expr {
 public:
  Cond c; Expr *l, *r;
  CompareExpr(Cond c, Expr *l, Expr *r) : c(c), l(l), r(r) {}
  EV gen(CG &cg) override;
  void genCond(CG &cg, BasicBlock *t, BasicBlock *f) override;
};

class LogicalExpr : public Expr {
 public:
  int op; Expr *l, *r;      // '&' for &&, '|' for ||
  LogicalExpr(int op, Expr *l, Expr *r) : op(op), l(l), r(r) {}
  EV gen(CG &cg) override;
  void genCond(CG &cg, BasicBlock *t, BasicBlock *f) override;
};

class CondExprNode : public Expr {
 public:
  Expr *c, *a, *b;
  CondExprNode(Expr *c, Expr *a, Expr *b) : c(c), a(a), b(b) {}
  EV gen(CG &cg) override;
};

class AssignExpr : public Expr {
 public:
  int op; Expr *lhs, *rhs;  // op 0 for plain '=', else the compound operator
  AssignExpr(int op, Expr *l, Expr *r) : op(op), lhs(l), rhs(r) {}
  EV gen(CG &cg) override;
};

class IncDecExpr : public Expr {
 public:
  bool prefix; int op; Expr *e;
  IncDecExpr(bool p, int op, Expr *e) : prefix(p), op(op), e(e) {}
  EV gen(CG &cg) override;
};

class CastExpr : public Expr {
 public:
  Type t; Expr *e;
  CastExpr(Type t, Expr *e) : t(t), e(e) {}
  EV gen(CG &cg) override;
};

class CallExpr : public Expr {
 public:
  std::string name; std::vector<Expr *> args;
  CallExpr(const char *n, std::vector<Expr *> a) : name(n), args(std::move(a)) {}
  EV gen(CG &cg) override;
 private:
  EV genIntrinsic(CG &cg, const IntrinsicDesc *d);
  EV genInline(CG &cg, class FnDecl *callee);
};

class Stmt : public Node {
 public:
  virtual void gen(CG &cg) = 0;
};

class Block : public Stmt {
 public:
  std::vector<Stmt *> stmts;
  void gen(CG &cg) override;
};

struct DeclItem { const char *name; Expr *init; int line; };

class DeclStmt : public Stmt {
 public:
  Type t; std::vector<DeclItem> items; bool isConst;
  DeclStmt(Type t, std::vector<DeclItem> i, bool c) : t(t), items(std::move(i)), isConst(c) {}
  void gen(CG &cg) override;
};

class ExprStmt : public Stmt {
 public:
  Expr *e;
  ExprStmt(Expr *e) : e(e) {}
  void gen(CG &cg) override;
};

class IfStmt : public Stmt {
 public:
  Expr *c; Stmt *t, *e;
  IfStmt(Expr *c, Stmt *t, Stmt *e) : c(c), t(t), e(e) {}
  void gen(CG &cg) override;
};

class WhileStmt : public Stmt {
 public:
  Expr *c; Stmt *body;
  WhileStmt(Expr *c, Stmt *b) : c(c), body(b) {}
  void gen(CG &cg) override;
};

class DoWhileStmt : public Stmt {
 public:
  Stmt *body; Expr *c;
  DoWhileStmt(Stmt *b, Expr *c) : body(b), c(c) {}
  void gen(CG &cg) override;
};

class ForStmt : public Stmt {
 public:
  Stmt *init; Expr *c, *step; Stmt *body;
  ForStmt(Stmt *i, Expr *c, Expr *s, Stmt *b) : init(i), c(c), step(s), body(b) {}
  void gen(CG &cg) override;
};

class SwitchStmt : public Stmt {
 public:
  Expr *e; Block *body;
  SwitchStmt(Expr *e, Block *b) : e(e), body(b) {}
  void gen(CG &cg) override;
};

class CaseLabel : public Stmt {   // `case K:` (value) or `default:` (null)
 public:
  Expr *value;
  CaseLabel(Expr *v) : value(v) {}
  void gen(CG &cg) override;
};

class ReturnStmt : public Stmt {
 public:
  Expr *e;
  ReturnStmt(Expr *e) : e(e) {}
  void gen(CG &cg) override;
};

class GotoStmt : public Stmt {
 public:
  std::string label;
  GotoStmt(const char *l) : label(l) {}
  void gen(CG &cg) override;
};

class LabelStmt : public Stmt {   // label: stmt
 public:
  std::string label; Stmt *s;
  LabelStmt(const char *l, Stmt *s) : label(l), s(s) {}
  void gen(CG &cg) override;
};

class BreakStmt : public Stmt {
 public:
  void gen(CG &cg) override;
};

class ContinueStmt : public Stmt {
 public:
  void gen(CG &cg) override;
};

struct Param { Type t; const char *name; bool ref = false; };

class FnDecl : public Node {
 public:
  Type ret; std::string name; std::vector<Param> params; Block *body; bool entry;
  bool isInline = false;   // expanded at every call; may take reference parameters
  const char *asmSpec = nullptr;   // extern ... asm("..."): a hand-written routine's register contract
  Function *fn = nullptr;
  FnDecl(Type r, const char *n, std::vector<Param> p, Block *b, bool e)
      : ret(r), name(n), params(std::move(p)), body(b), entry(e) {}
};

class ConstDecl : public Node {
 public:
  Type t; DeclItem item;
  ConstDecl(Type t, DeclItem i) : t(t), item(i) {}
};

class TransUnit {
 public:
  std::vector<FnDecl *> fns;
  std::vector<ConstDecl *> consts;
};

// Builds the TAC of a translation unit (ast.cpp).
Program *generateTAC(TransUnit *tu);

#endif
