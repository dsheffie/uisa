// -*-c++-*-
/* ast.h: the abstract syntax tree of the millicode C subset.
 *
 * The node hierarchy is Node / Expr / Stmt / declarations, and each node
 * emits its own TAC. Semantic checks happen during TAC generation (ast.cpp),
 * which also applies C's integer promotions and conversions so that the TAC
 * is plain 32-bit arithmetic. 64-bit values are
 * pairs of 32-bit words from here on: the back end never sees them.
 */
#ifndef UCC_AST_H
#define UCC_AST_H

#include <cstdint>
#include <map>
#include <memory>
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
  // program mode: pointers (to *sub) and arrays (arrLen elements of *sub;
  // arrLen -1 for `[]` until an initializer sizes it)
  bool ptr = false, arr = false;
  int arrLen = 0;
  // the C keywords this type was spelled with (for `unsigned long`, ...):
  // 1 unsigned, 2 signed, 4 int, 8 short, 16 long, 32 long long, 64 char
  int kw = 0;
  std::shared_ptr<Type> sub;
  Type() {}
  Type(int bits, bool sign) : bits(bits), sign(sign) {}
  static Type voidType() { Type t; t.isVoid = true; return t; }
  static Type s32() { return Type(32, true); }
  static Type u32() { return Type(32, false); }
  static Type ptrTo(const Type &t) {
    Type p(32, false);
    p.ptr = true;
    p.sub = std::make_shared<Type>(t);
    return p;
  }
  static Type arrayOf(const Type &t, int n) {
    Type a(32, false);
    a.arr = true;
    a.arrLen = n;
    a.sub = std::make_shared<Type>(t);
    return a;
  }
  bool isPtr() const { return ptr; }
  bool isArray() const { return arr; }
  bool isInt() const { return !isVoid && !sd && !ptr && !arr; }     // an integer type
  bool isScalar() const { return isInt() || ptr; }
  bool is64() const { return isInt() && bits == 64; }
  bool isStruct() const { return sd != nullptr && !ptr && !arr; }
  const Type &target() const { return *sub; }   // pointee / element
  int words() const;          // 32-bit words a value of this type occupies
  std::string str() const;
  bool operator==(const Type &o) const {
    if (ptr != o.ptr || arr != o.arr) return false;
    if (ptr) return *sub == *o.sub;
    if (arr) return arrLen == o.arrLen && *sub == *o.sub;
    return isVoid == o.isVoid && sd == o.sd && (sd || (bits == o.bits && sign == o.sign));
  }
};
// Program mode memory layout (big-endian, ILP32; 64-bit integers are two
// words, high word first, 4-byte aligned).
int sizeOf(const Type &t);
int alignOf(const Type &t);
bool hasArray(const Type &t);   // an array, or a struct with an array inside

// struct name { fields }: plain value semantics; fields are scalars, structs
// or (program mode) arrays.
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

// An lvalue: a register variable (sym), or memory at an address that is
// absolute (off), frame-relative (fobj + off) or base register + off.
struct LV {
  Type ty;
  Sym *sym = nullptr;
  Operand base;
  int32_t off = 0;
  int fobj = -1;
  bool ok = false;
};

class Expr : public Node {
 public:
  virtual EV gen(CG &cg) = 0;
  // Branch to t if the expression is nonzero, else to f.
  virtual void genCond(CG &cg, BasicBlock *t, BasicBlock *f);
  // The variable (or struct field) an assignment writes, or null.
  virtual Sym *lvalueSym(CG &cg) { (void)cg; return nullptr; }
  // Program mode: the object the expression designates (ok false if none).
  virtual bool isLvalue() const { return false; }
  virtual LV lvalue(CG &cg);
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
  bool isLvalue() const override { return true; }
  LV lvalue(CG &cg) override;
};

class MemberExpr : public Expr {   // base.field
 public:
  Expr *base; std::string field;
  MemberExpr(Expr *b, const char *f) : base(b), field(f) {}
  EV gen(CG &cg) override;
  Sym *lvalueSym(CG &cg) override;
  bool isLvalue() const override { return base->isLvalue(); }
  LV lvalue(CG &cg) override;
};

// program mode: *p, &x, a[i] (p->f is (*p).f), the comma operator
class DerefExpr : public Expr {
 public:
  Expr *e;
  DerefExpr(Expr *e) : e(e) {}
  EV gen(CG &cg) override;
  bool isLvalue() const override { return true; }
  LV lvalue(CG &cg) override;
};

class AddrOfExpr : public Expr {
 public:
  Expr *e;
  AddrOfExpr(Expr *e) : e(e) {}
  EV gen(CG &cg) override;
};

class IndexExpr : public Expr {
 public:
  Expr *a, *i;
  IndexExpr(Expr *a, Expr *i) : a(a), i(i) {}
  EV gen(CG &cg) override;
  bool isLvalue() const override { return true; }
  LV lvalue(CG &cg) override;
};

class CommaExpr : public Expr {
 public:
  Expr *l, *r;
  CommaExpr(Expr *l, Expr *r) : l(l), r(r) {}
  EV gen(CG &cg) override;
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

// sizeof(type) or sizeof expr (program mode): a u32 constant. The operand
// is not evaluated.
class SizeofExpr : public Expr {
 public:
  Type t; std::vector<Expr *> dims; Expr *e;   // a type (with array dims), or e
  SizeofExpr(Type t, std::vector<Expr *> d) : t(t), dims(std::move(d)), e(nullptr) {}
  explicit SizeofExpr(Expr *e) : e(e) {}
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

// A braced initializer: `{ a, { b, c }, ... }` (program mode).
struct InitList {
  std::vector<std::pair<Expr *, InitList *>> items;   // one of the two is set
};

// One declarator: `**name[3][4] = init`. ptrs and dims are program mode.
struct DeclItem {
  const char *name; Expr *init; int line;
  int ptrs = 0;
  std::vector<Expr *> dims;   // null for `[]`
  InitList *list = nullptr;   // a braced initializer instead of init
};

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
  bool proto = false;      // a declaration without a body (program mode)
  bool sret = false;       // program mode: returns more than two words through a hidden pointer
  FnDecl(Type r, const char *n, std::vector<Param> p, Block *b, bool e)
      : ret(r), name(n), params(std::move(p)), body(b), entry(e) {}
};

class ConstDecl : public Node {
 public:
  Type t; DeclItem item;
  ConstDecl(Type t, DeclItem i) : t(t), item(i) {}
};

// A global variable declaration (program mode): storage in the data section.
class GlobalDecl : public Node {
 public:
  Type t; std::vector<DeclItem> items;
  GlobalDecl(Type t, std::vector<DeclItem> i) : t(t), items(std::move(i)) {}
};

class TransUnit {
 public:
  std::vector<FnDecl *> fns;
  std::vector<ConstDecl *> consts;
  std::vector<GlobalDecl *> globals;
};

// Builds the TAC of a translation unit (ast.cpp).
Program *generateTAC(TransUnit *tu);

#endif
