// -*-c++-*-
/* emit.h: uisa assembly emission shared by millicode and program mode. */
#ifndef UCC_EMIT_H
#define UCC_EMIT_H

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "tac.h"

namespace spec {
#include "uisa_intrinsics.inc"
}

struct AsmLine {
  enum Kind { Text, Label, Branch, Jump } kind = Text;
  std::string text;              // Text: the instruction; Label: the name
  Cond cond = Cond::EQ;          // Branch
  std::string rs1, rs2, target;  // Branch / Jump
  int line = 0;
};

class Emitter {
 public:
  std::vector<AsmLine> out;
  int curLine = 0;
  void text(const std::string &s) {
    AsmLine l;
    l.text = s;
    l.line = curLine;
    out.push_back(l);
  }
  void label(const std::string &s) {
    AsmLine l;
    l.kind = AsmLine::Label;
    l.text = s;
    out.push_back(l);
  }
  void branch(Cond c, const std::string &a, const std::string &b, const std::string &t) {
    // uisa has eq ne lt ge ltu geu; the others swap their operands
    std::string x = a, y = b;
    if (c == Cond::GT || c == Cond::LE || c == Cond::GTU || c == Cond::LEU) {
      std::swap(x, y);
      c = swapCond(c);
    }
    AsmLine l;
    l.kind = AsmLine::Branch;
    l.cond = c;
    l.rs1 = x;
    l.rs2 = y;
    l.target = t;
    l.line = curLine;
    out.push_back(l);
  }
  void jump(const std::string &t) {
    AsmLine l;
    l.kind = AsmLine::Jump;
    l.target = t;
    l.line = curLine;
    out.push_back(l);
  }
  void mov(const std::string &d, const std::string &s) {
    if (d != s) text("add " + d + ", " + s + ", x0");
  }
  void li(const std::string &d, int32_t v) {
    if ((v >= spec::ADDI_LO && v <= spec::ADDI_HI)) {
      text("addi " + d + ", x0, " + std::to_string(v));
      return;
    }
    uint32_t u = (uint32_t)v;
    uint32_t lo = u & ((1u << spec::LUI_SHIFT) - 1);
    text("lui " + d + ", " + std::to_string(u >> spec::LUI_SHIFT));
    if (lo) text("ori " + d + ", " + d + ", " + std::to_string(lo));
  }
  // Sequentialize a parallel copy {dst <- src}. A cycle of three or more
  // moves goes through a free register from `temps` when there is one (k + 1
  // moves); otherwise, and for two-move cycles, it is broken with xor swaps
  // (three instructions per swap), which need no scratch register.
  void parallelCopy(std::vector<std::pair<std::string, std::string>> moves,
                    const std::vector<std::string> &temps = {}) {
    auto dropTrivial = [&] {
      moves.erase(std::remove_if(moves.begin(), moves.end(), [](auto &m) { return m.first == m.second; }),
                  moves.end());
    };
    dropTrivial();
    while (!moves.empty()) {
      bool progress = false;
      for (size_t i = 0; i < moves.size(); i++) {
        const std::string &d = moves[i].first;
        bool isSrc = false;
        for (size_t j = 0; j < moves.size(); j++)
          if (j != i && moves[j].second == d) isSrc = true;
        if (isSrc) continue;
        mov(d, moves[i].second);
        moves.erase(moves.begin() + i);
        progress = true;
        break;
      }
      if (progress) continue;
      // every remaining destination is also a source: cycles only
      std::string d = moves[0].first, s = moves[0].second;
      size_t len = 1;
      for (std::string c = s; c != d && len <= moves.size(); len++)
        for (auto &m : moves)
          if (m.first == c) {
            c = m.second;
            break;
          }
      if (len >= 3 && !temps.empty()) {
        mov(temps[0], d);              // d's old value moves aside; d is free
        for (auto &m : moves)
          if (m.second == d) m.second = temps[0];
        continue;
      }
      text("xor " + d + ", " + d + ", " + s);
      text("xor " + s + ", " + s + ", " + d);
      text("xor " + d + ", " + d + ", " + s);
      moves.erase(moves.begin());
      for (auto &m : moves)          // old d now lives in s
        if (m.second == d) m.second = s;
      dropTrivial();
    }
  }
};


std::string R(const Operand &o);
std::string R(Reg *r);
std::string blockLabel(BasicBlock *b);
// One non-terminator instruction (Call is millicode's `call`; program mode
// lowers its own calls). `next` is the following instruction (CallRes).
void emitInstr(Emitter &E, Instr *i, Instr *next);
// Give every edge into a block with phis from a block with several
// successors a block of its own.
void splitCriticalEdges(Function *f);
void emitLayout(Function *f, std::map<BasicBlock *, std::vector<AsmLine>> &body, Emitter &E, FILE *out,
                const std::function<void(Emitter &, Instr *)> &ret);

#endif
