// -*-c++-*-
/* tac.h: the three-address code of the uisa millicode compiler.
 *
 * Forked from Decaf's tac.h. Decaf's TAC named stack slots (Location) and
 * had a separate renamed form (rVariable); here one Reg class serves both:
 * before SSA construction a Reg is a source variable that may be assigned
 * many times, afterwards every Reg has exactly one definition. All values
 * are 32 bits; the front end has already applied C's conversions and split
 * 64-bit values into pairs of 32-bit words.
 */
#ifndef UCC_TAC_H
#define UCC_TAC_H

#include <cstdint>
#include <set>
#include <string>
#include <vector>

struct IntrinsicDesc {
  const char *name;
  const char *asmfmt;     // %d result, %N argument N, %mN address argument N as disp(base)
  const char *args;       // per argument: v value, i constant, m address, t tied old result
  int has_result;
  int tied;               // argument whose register the result must reuse, or -1
  int side_effects;
  int noreturn;
  int flags;              // reads or writes the renamed guest flags
  struct { int lo, hi, scale; } range[6];
};
struct NamedConst { const char *name; int value; };
struct EntryReg { const char *name; int reg; };

class Function;
class BasicBlock;
class Instr;

class Reg {
 public:
  int id;
  std::string name;
  Function *fn;
  Instr *def = nullptr;       // SSA definition
  Reg *var = nullptr;         // SSA: the source variable this value renames
  int color = -1;             // assigned x register
  int precolor = -1;          // fixed x register (entry-ABI values)
  bool isVar = false;         // a source variable (before SSA)
  Reg(int id, std::string name, Function *fn) : id(id), name(std::move(name)), fn(fn) {}
  std::string str() const { return name + "." + std::to_string(id); }
};

struct Operand {
  enum Kind { None, R, Imm, Phys } k = None;
  Reg *r = nullptr;
  int32_t imm = 0;
  int phys = 0;               // register namespace index (x0, d3, gpc, ...)
  static Operand reg(Reg *r) { Operand o; o.k = R; o.r = r; return o; }
  static Operand cst(int32_t v) { Operand o; o.k = Imm; o.imm = v; return o; }
  static Operand preg(int p) { Operand o; o.k = Phys; o.phys = p; return o; }
  bool isReg() const { return k == R; }
  bool isImm() const { return k == Imm; }
  std::string str() const;
};

enum class Cond { EQ, NE, LT, GE, LTU, GEU, GT, LE, GTU, LEU };
Cond invert(Cond c);
Cond swapCond(Cond c);
const char *condName(Cond c);
bool evalCond(Cond c, uint32_t a, uint32_t b);

enum class Op {
  Copy,                                          // dst = a0
  Add, Sub, Mul, Div, DivU, Rem, RemU,
  And, Or, Xor, Shl, Shr, Sar,
  Set,                                           // dst = (a0 cond a1)
  Sext8, Sext16, Zext8, Zext16,
  Intrin,                                        // [dst =] intrinsic(a...)
  Call,                                          // [dst =] callee(a...)
  CallRes,                                       // dst = second result word of the Call just before
  Phi,                                           // dst = phi(a[i] from preds[i])
  PWrite,                                        // phys register `phys` = a0
  // terminators
  Br,                                            // if (a0 cond a1) goto t else f
  Jmp,                                           // goto t
  Switch,                                        // goto targets[(a0 >> swLsb) & (2^swWidth - 1)] (bx)
  Ret,                                           // return [a0 [, a1]]
  Stop,                                          // control ended in a noreturn intrinsic
};

class Instr {
 public:
  Op op;
  Reg *dst = nullptr;
  std::vector<Operand> a;
  Cond cond = Cond::EQ;
  const IntrinsicDesc *in = nullptr;
  Function *callee = nullptr;
  BasicBlock *t = nullptr, *f = nullptr;
  std::vector<BasicBlock *> targets;             // Switch: one per table slot (2^swWidth)
  int swLsb = 0, swWidth = 0;                    // Switch: the index field of a0
  std::vector<BasicBlock *> preds;               // phi incoming blocks, parallel to a
  int phys = 0;                                  // PWrite target
  int line = 0;
  int disp = 0;                                  // displacement folded into the address argument
  bool falloff = false;                          // Stop: control reached the end of the function
  Instr(Op op) : op(op) {}
  bool isTerm() const { return op == Op::Br || op == Op::Jmp || op == Op::Switch || op == Op::Ret || op == Op::Stop; }
  bool hasSideEffects() const;
  std::string str() const;
};

class BasicBlock {
 public:
  int id;
  std::string name;
  Function *fn;
  std::vector<Instr *> ins;
  std::vector<BasicBlock *> preds, succs;
  // dominance (codegen.cpp)
  BasicBlock *idom = nullptr;
  std::vector<BasicBlock *> dtree;
  std::vector<BasicBlock *> dfrontier;
  std::set<Reg *> phiVars;
  int loopDepth = 0;
  int rpo = 0;
  BasicBlock(int id, std::string name, Function *fn) : id(id), name(std::move(name)), fn(fn) {}
  Instr *term() const { return ins.empty() ? nullptr : ins.back(); }
  bool terminated() const { return term() && term()->isTerm(); }
  std::string label() const { return name + "." + std::to_string(id); }   // unique, for dumps
};

class Function {
 public:
  std::string name;
  bool isEntry = false;          // millicode entry point (dispatched, never called)
  bool isAsm = false;            // hand-written routine declared with asm("..."): no body, fixed registers
  int asmArgs = 0;               // asm: declared argument words; params after them are entry-ABI registers it reads
  int asmDepth = 1;              // asm: return-stack entries it uses, its own call included
  std::vector<Reg *> clobberRegs;  // asm: the other registers it may write (precolored)
  bool returnsValue = false;
  int retWords = 0;              // 32-bit words returned (2 for 64-bit types)
  int line = 0;
  std::vector<Reg *> params;     // SSA values defined at entry (after construction)
  std::vector<Reg *> paramVars;  // the source variables of the parameters
  std::vector<Reg *> entryRegs;  // entry-ABI values (precolored), defined at entry
  std::vector<Reg *> retRegs;    // the registers the result words are returned in
  std::vector<BasicBlock *> blocks;
  std::vector<Reg *> regs;
  std::set<Function *> callees;
  int nextBlock = 0;
  Function(std::string name) : name(std::move(name)) {}
  Reg *newReg(const std::string &n) {
    Reg *r = new Reg((int)regs.size(), n, this);
    regs.push_back(r);
    return r;
  }
  BasicBlock *newBlock(const std::string &n) {
    BasicBlock *b = new BasicBlock(nextBlock++, n, this);
    blocks.push_back(b);
    return b;
  }
  void print(FILE *f) const;
  void rebuildEdges();
};

struct Program {
  std::vector<Function *> funcs;
};

const IntrinsicDesc *findIntrinsic(const std::string &name);
std::vector<NamedConst> specConsts();
const EntryReg *findEntryReg(const std::string &name);
const char *regName(int idx);

#endif
