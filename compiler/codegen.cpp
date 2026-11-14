/* File: codegen.cc
 *
 * Implementation for the CodeGenerator class. The methods don't do anything
 * too fancy, mostly just create objects of the various Tac instruction
 * classes and append them to the list.
 */

#include "codegen.h"

#include <list>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <iostream>

#include <limits.h>
#include <execinfo.h>
#include <sys/types.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tac.h"
#include "mips.h"
#include "options.h"

#define MAX_STR 255
#define print_var(x) { std::cout << #x << " = " << x << std::endl;}

extern bool enable_float;

static std::map<std::string, llvm::Function*> _builtinFuncs;
static std::map<std::string, llvm::Value*> labelMap;
static std::map<std::string, std::vector<std::string> >vtableNames;
static std::map<std::string, llvm::Value*> globalStringMap;
static std::map<std::string, Variable*> globalVariableMap;
static std::map<std::string, rVariable*> globalVariableValues;

//hint from review section
CodeGenerator* cg;

void BasicBlock::add_succ(BasicBlock *s) 
{
  if(hasReturn()) {
    //block with return can have no successors..
    return;
  }

  if(succs.find(s) != succs.end())
    return;

  if(hasGoto() && succs.size()==1) {
    printf("block with a goto can have no more than 1 sucessor\n");
    printf("trying to add %s\n", s->name.c_str());
    Print();
  
    exit(-1);
  }
  succs.insert(s);
  s->preds.insert(this);
}


llvm::Value *rVariable::getLLVMValue(llvm::IRBuilder<> *myIRBuilder,
				     llvm::LLVMContext *Context)
    {
      if(l->isGlobal())
	{
	  //printf("global get...\n");
	  //myValue->dump();
	  //abort();
	  return myIRBuilder->CreateLoad(myValue);
	}
      else
	{
	  if(!myValue) 
	    {
	      llvm::Type *i32 = llvm::Type::getInt32Ty(*Context);
	      myValue = llvm::ConstantInt::get(i32,-1);
	    }	
	  return myValue;
	}
    }

void rVariable::setLLVMValue(llvm::IRBuilder<> *myIRBuilder,
			     llvm::LLVMContext *Context,
			     llvm::Value *v)
{
  if(l->isGlobal())
    {
      //printf("global set...\n");
      myIRBuilder->CreateStore(v, myValue);
      //myValue->dump();
      //abort();
    }
  else
    {
      myValue = v;
    }
}


rVariable *getNewName(Variable *v, 
	     std::map<Variable*, int64_t> &counters,
	     std::map<Variable*, std::list<rVariable*> > &stacks)
{
  if(!v) 
    {
#ifdef __linux__
      char pid_buf[80] = {0};
      sprintf(pid_buf, "%d", getpid());
      char name_buf[512];
      /* term string */
      name_buf[readlink("/proc/self/exe", name_buf, 511)]=0;
      pid_t child = fork();
      if(!child)
	{
	  dup2(2,1);
	  fprintf(stdout, "stack trace for %s\n", name_buf);
	  execlp("gdb", "gdb", "--batch", "-n", "-ex", "thread", "-ex", "bt", 
		 name_buf, pid_buf, NULL);
	  abort();
	}
      else
	{
	  waitpid(child, NULL, 0);
	}
#endif
      exit(-1);
    }
  int64_t i = counters[v];
  counters[v] = i + 1;
  rVariable *rv = new rVariable(i, v);
  stacks[v].push_back(rv);
  return rv;
}


rVariable *BasicBlock::getUniqVar(Location *l,
				  std::map<Location*, Variable*> &vMap,
				  std::map<Variable*, int64_t> &counters,
				  std::map<Variable*, std::list<rVariable*> > &stacks
				  )
{
  Variable *v = vMap[l];
  std::map<Variable*, rVariable*>::iterator mit = rMap.find(v);
  rVariable *rv = 0;
  if(mit != rMap.end())
    {
      rv = mit->second;
    }
 
  if(!rv)
    {
      rv = rMap[v] = getNewName(v, counters, stacks);
    }
  return rv;
}

rVariable* getSrcVar(Location *l, 
		     BasicBlock *bb,
		     std::map<Location*, Variable*> &variableMap)
{
  rVariable *rv = 0;
  if(l->isGlobal())
    {
      const char *vName = l->GetName();
      rv = globalVariableValues[l->GetName()];
      if(!rv) { printf("line %d\n", __LINE__); abort(); }
    }
  else
    {
      Variable *v = variableMap[l];
      if(!v)  { printf("line %d\n", __LINE__); abort(); }
      rv = bb->rMap[v];
      //if(!rv) { printf("line %d\n", __LINE__); abort(); }
    }
  return rv;
}

rVariable* getDstVar(Location *l, 
		     BasicBlock *bb,
		     std::map<Variable*, int64_t> &counters,
		     std::map<Variable*, std::list<rVariable*> > &stacks,
		     std::map<Location*, Variable*> &variableMap)
{
  rVariable *rv = 0;
  if(l->isGlobal())
    {
      rv = globalVariableValues[l->GetName()];
      if(!rv) { printf("line %d\n", __LINE__); abort(); }
    }
  else
    {
      Variable *v = variableMap[l];
      if(!v)  { printf("line %d\n", __LINE__); abort(); }
      rv = getNewName(v, counters, stacks);
      if(!rv)  { printf("line %d\n", __LINE__); abort(); }
      bb->uassigns.push_back(rv);
      bb->rMap[v] = rv;
    }
  return rv;
}
		     


void reName(BasicBlock *bb, 
	    std::map<Variable*, int64_t> &counters,
	    std::map<Variable*, std::list<rVariable*> > &stacks,
	    std::map<Location*, Variable*> &variableMap)
{
  std::map<Variable*, std::list<rVariable*> > bstacks = stacks;

  //printf("bb->name = %s\n", bb->name.c_str()); 
  //std::map<Variable*, rVariable*> rMap;
  for(std::map<Variable*, std::list<rVariable*> >::iterator mit = stacks.begin(); 
      mit != stacks.end(); mit++)
    {
      Variable *v = mit->first;
      if(mit->second.size() > 0)
	{
	  rVariable *rv = mit->second.back();
	  bb->rMap[v] = rv;
	}
      else
	{
	  bb->rMap[v] = 0;
	}
    }

  /* define all argument variables in the entry bb */
  if(bb->isEntryBlock)
    {
      for(std::map<Location*, Variable*>::iterator mit = variableMap.begin();
	  mit != variableMap.end(); mit++)
	{
	  Location *l = mit->first;
	  if(!l) continue;
	  if(l->isFuncArg) 
	    {
	      Variable *p = mit->second;
	      if(!p) 
		abort();
	      rVariable *rv = getNewName(p, counters, stacks);
	      bb->rMap[p] = rv;
	      bb->funcArgs.insert(rv);
	      rv->name = l->funcArgName;
	    }
	}
    }

  /* handle phi variables */
  for(std::set<Variable*>::iterator sit = bb->phis.begin();
      sit != bb->phis.end(); sit++)
    {
      Variable *p = *sit;
      rVariable *rv = getNewName(p, counters, stacks);
      bb->uphis.insert(rv);
      bb->rMap[p] = rv;
    }

  /* iterate over instructions and rename operands */
  for(std::vector<Instruction*>::iterator it = bb->ins.begin();
      it != bb->ins.end(); it++)
    {
      Instruction *insn = *it;
      const Instruction::TACTypes tacType = insn->getType();
      switch(tacType)
	{
	case Instruction::LoadConstant:
	  {
	    LoadConstant *LC = dynamic_cast<LoadConstant*>(insn);
	    Location *dst = LC->dst;
	    Variable *d = variableMap[dst];
	    if(!d) abort();
	    rVariable *rv = getNewName(d, counters, stacks);
	    bb->uassigns.push_back(rv);
	    bb->rMap[d] = rv;
	    LC->rDst = rv;
	    break;
	  }
	case Instruction::LoadStringConstant:
	  {
	    LoadStringConstant *LSC = dynamic_cast<LoadStringConstant*>(insn);
	    Location *dst = LSC->dst;
	    Variable *d = variableMap[dst];
	    if(!d) abort();
	    rVariable *rv = getNewName(d, counters, stacks);
	    bb->uassigns.push_back(rv);
	    bb->rMap[d] = rv;
	    LSC->rDst = rv;
	    break;
	  }
	case Instruction::LoadLabel:
	  {
	    LoadLabel *LL = dynamic_cast<LoadLabel*>(insn);
	    Location *dst = LL->dst;
	    Variable *d = variableMap[dst];
	    if(!d) abort();
	    rVariable *rv = getNewName(d, counters, stacks);
	    bb->uassigns.push_back(rv);
	    bb->rMap[d] = rv;
	    LL->rDst = rv;
	    break;
	  }
	case Instruction::Assign:
	  {
	    Assign *A = dynamic_cast<Assign*>(insn);
	    A->rSrc = getSrcVar(A->src, bb, variableMap);
	    A->rDst = getDstVar(A->dst, bb, counters, stacks, variableMap);

	    //if(A->dst->GetSegment()!=gpRelative)
	    //{
	    //	Variable *d = variableMap[A->dst];
	    //	if(!d) {
	    //	  A->Print();
	    //	  exit(-1);
	    //	}
	    //	rVariable *rv = getNewName(d, counters, stacks);
	    //	bb->uassigns.push_back(rv);
	    //	bb->rMap[d] = rv;
	    //	A->rDst = rv;
	    // }

	    break;
	  }
	case Instruction::Load:
	  {
	    Load *L = dynamic_cast<Load*>(insn);
	    L->rSrc = getSrcVar(L->src, bb, variableMap);
	    
	    Location *dst = L->dst;
	    Variable *d = variableMap[dst];
	    if(!d) abort();
	    rVariable *rv = getNewName(d, counters, stacks);
	    bb->uassigns.push_back(rv);
	    bb->rMap[d] = rv;
	    L->rDst = rv;
	    break;
	  }
	case Instruction::Store:
	  {
	    //dst is the address
	    //src is the value
	    Store *S = dynamic_cast<Store*>(insn);
	    S->rSrc = getSrcVar(S->src, bb, variableMap);
	    S->rDst = getSrcVar(S->dst, bb, variableMap);
	    break;
	  }
	case Instruction::BinaryOp:
	  {
	    BinaryOp *B = dynamic_cast<BinaryOp*>(insn);
	    B->rOp1 = getSrcVar(B->op1, bb, variableMap);
	    B->rOp2 = getSrcVar(B->op2, bb, variableMap);
	    Location *dst = B->dst;
	    Variable *d = variableMap[dst];
	    if(!d) { printf("%d\n", __LINE__); exit(-1); }
	    rVariable *rv = getNewName(d, counters, stacks);
	    bb->uassigns.push_back(rv);
	    bb->rMap[d] = rv;
	    B->rDst = rv;
	    break;
	  }
	case Instruction::Label:
	  break;
	case Instruction::Goto:
	  break;
	case Instruction::IfZ:
	  {
	    IfZ *Z = dynamic_cast<IfZ*>(insn);
	    Z->rTest = getSrcVar(Z->test, bb, variableMap);
	    break;
	  }
	case Instruction::BeginFunc:
	  break;
	case Instruction::EndFunc:
	  break;
	case Instruction::Return:
	  {
	    Return *R = dynamic_cast<Return*>(insn);
	    if(R->val) {
	      Variable *v = variableMap[R->val];
	      if(!v)  { printf("%d\n", __LINE__); exit(-1); }
	      R->rVal = bb->rMap[v];
	    } else {
	      R->rVal = 0;
	    }
	    break;
	  }
	case Instruction::PushParam:
	  {
	    PushParam *PP = dynamic_cast<PushParam*>(insn);
	    PP->rParam = getSrcVar(PP->param, bb, variableMap);
	    
	    if(!PP->rParam) {
	      PP->rParam = bb->getUniqVar(PP->param,variableMap,
					  counters, stacks);
	    }

	    if(!PP->rParam) 
	      {
		PP->Print();  
		printf("LINE %d\n", __LINE__); 
		exit(-1); 
	      }
	    break;
	  }
	case Instruction::PopParams:
	  break;
	case Instruction::LCall:
	  {
	    LCall *L = dynamic_cast<LCall*>(insn);
	    Location *dst = L->dst;
	    if(dst)
	      {
		Variable *d = variableMap[dst];
		if(!d) {
		  insn->Print();
		  exit(-1);
		}
		//assert(d);
		rVariable *rv = getNewName(d, counters, stacks);
		bb->uassigns.push_back(rv);
		bb->rMap[d] = rv;
		L->rDst = rv;
	      }
	    break;
	  }
	case Instruction::ACall:
	  {
	    ACall *A = dynamic_cast<ACall*>(insn);
	    Variable *v = variableMap[A->methodAddr];
	    A->rMethodAddr = bb->rMap[v];
	    Location *dst = A->dst;
	    if(dst)
	      {
		Variable *d = variableMap[dst];
		rVariable *rv = getNewName(d, counters, stacks);
		bb->uassigns.push_back(rv);
		bb->rMap[d] = rv;
		A->rDst = rv;
	      }
	    break;
	  }
	case Instruction::VTable:
	  printf("vtable???\n");
	  exit(-1);
	  break;
	default:
	  break;
	}

      /* rename dest */

    }

 
 
  /* set "appropriate" phi parameters */
  for(BasicBlock* nbb : bb->dtree_succs) {
    reName(nbb,counters,stacks, variableMap);
  }
  stacks = bstacks;
}

void reNameVariables(std::vector<BasicBlock*> &basicBlocks,
		     std::vector<Variable*> &variables,
		     std::map<Location*, Variable*> &variableMap)
{
  std::map<Variable*, int64_t> counters;
  std::map<Variable*, std::list<rVariable*> > stacks; 
  std::map<std::string, Variable*> vMap;

  for(Variable* var : variables){
    counters[var] = 0;
    vMap[var->name] = var;
  }

  reName(basicBlocks[0]->parentFunc->entry,
	 counters,stacks,variableMap);
  
  /* update phis */
   for(std::vector<BasicBlock*>::iterator it0 = basicBlocks.begin();
      it0 != basicBlocks.end(); it0++)
    {
      BasicBlock *bb0 = *it0;
      //iterate over block successors
      for(std::set<BasicBlock*>::iterator it1 = bb0->succs.begin();
	  it1 != bb0->succs.end(); it1++)
	{
	  BasicBlock *bb1 = *it1;
	  //iterate over phi variables in 
	  //successors
	  for(std::set<rVariable*>::iterator sit0 = bb1->uphis.begin();
	      sit0 != bb1->uphis.end(); sit0++)
	    {
	      //if we've updated that variable
	      //in our basicblock, update phi with
	      //appropriate link
	      Variable *p = (*sit0)->base;
	      std::map<Variable*, rVariable*>::iterator mit = 
		bb0->rMap.find(p);

	      rVariable *rp = *sit0;
	      if(mit == bb0->rMap.end()) 
		{
		  rp->phiEdges[bb0] = 0;
		}
	      else
		{
		  //printf("BB %s adds edge into %s for %s\n", 
		  //bb0->name.c_str(), bb1->name.c_str(), rp->getName().c_str());
		  rp->phiEdges[bb0] = bb0->rMap[p];
		}
	    }
	}
    }
  
}


void insertPhis(std::vector<BasicBlock*> &basicBlocks, std::vector<Variable*> &variables) {
  using namespace std;
  map<BasicBlock*, set<string>> liveIn, liveOut, uses, defs;

  bool changed = false;
  for(auto bb : basicBlocks) {
    std::set<std::string> u, d;
    for(auto insn : bb->ins) {
      /* variables used before definition */
      for(size_t i=0, n = insn->numOperands(); i < n; i++) {
	auto *l = insn->getLocation(i);
	if(l==nullptr) {
	  std::cout << "nullptr @ " << i<< " : ";
	  insn->Print();
	  continue;
	}
	std::string src = insn->getLocation(i)->GetName();
	if(d.find(src) == d.end())
	  u.insert(src);
      }
      /* variables defined before used */
      if(insn->getDst()) {
	std::string dst = insn->getDst()->GetName();
	if(u.find(dst) == u.end())
	  d.insert(dst);
      }
    }
    uses[bb] = u;
    defs[bb] = d;
  }

  /* live info */
#if 0
  for(auto bb : basicBlocks) {
    cout << bb->getName() << "\n";
    cout << "\t uses:"; 
    for(auto u : uses[bb]) {
      cout << u <<" ";
    }
    cout << "\n";
    cout << "\t defs:";
    for(auto d : defs[bb]) {
      cout << d <<" ";
    }
    cout << "\n";
  }
#endif

  do {
    changed = false;
    for(auto bb : basicBlocks) {
      set<string> in, out;

      for(auto sbb :  bb->succs) {
	for(auto var : liveIn[sbb]) {
	  out.insert(var);
	}
      }

      if(liveOut[bb] != out) {
	changed = true;
	liveOut[bb] = out;
      }
      in = out;
      
      for(auto dit = defs[bb].begin(); dit != defs[bb].end(); dit++) {
	auto pit = in.find(*dit);
	if(pit != in.end())
	  in.erase(pit);
      }
      
      for(auto u : uses[bb]) {
	in.insert(u);
      }

      if(liveIn[bb] != in) {
	changed = true;
	liveIn[bb] = in;
      }

    }
  }
  while(changed);

#if 0
  for(auto bb : basicBlocks) {
    cout << bb->getName() << "\n";
    cout << "\t liveOut:"; 
    for(auto lo : liveOut[bb]) {
      cout << lo <<" ";
    }
    cout << "\n";
  }
#endif
  
  /* theorem: if S is the set of basicblocks that define variable V,
   * then DF(S) is the set of basicblocks that require a phi function
   * for V */
  
  for(auto v : variables) {
    bool updated = true;
    std::string varName = v->getName();
    while(updated){
      updated = false;
      /* iterate over blocks in which v is defined */
      for(auto bb : v->assignedIn) {
	/* no need to generate phis if the variable
	 * is not live-out of the basicblock */
	if(liveOut[bb].find(varName) == liveOut[bb].end()) {
	  continue;
	}
	  /* any blocks in dominance frontier require a phi */
	for(auto d : bb->dfrontier)  {
	  updated |= v->addAssign(d);
	  updated |= d->addPhi(v);
	  if(updated) 
	      break;
	}
	if(updated)
	  break;
      }
    }
  }
}


std::set<BasicBlock*> intersect(std::set<BasicBlock*> &x, 
				std::set<BasicBlock*> &y) {
  std::set<BasicBlock*> r;
  if(x.size() < y.size())
    {
      for(std::set<BasicBlock*>::iterator sit = x.begin();
	  sit != x.end(); sit++)
	{
	  BasicBlock *b = *sit;
	  if(y.find(b) != y.end()) {
	    r.insert(b);
	  }
	}
    }
  else
    {
      for(std::set<BasicBlock*>::iterator sit = y.begin();
	  sit != y.end(); sit++) 
	{
	  BasicBlock *b = *sit;
	  if(x.find(b) != x.end()) {
	    r.insert(b);
	  }
	}
    }
  assert(r.size() <= x.size());
  assert(r.size() <= y.size());
  return r;
}

class LengauerTarjanDominators {
private:
  std::vector<BasicBlock*> &blocks;
  BasicBlock *root = nullptr;
  ssize_t dfsCounter = 1;
  /* Parent in the DFS tree */
  std::map<BasicBlock*, BasicBlock*> Parent;
  /* Ancestor chain in DFS tree */
  std::map<BasicBlock*, BasicBlock*> Ancestor;
  std::map<BasicBlock*, BasicBlock*> Label;
  std::map<BasicBlock*, std::set<BasicBlock*>> Buckets;
  std::map<ssize_t, BasicBlock*> Ndfs;
  std::map<BasicBlock*, ssize_t> Sdno;
  
  void DFS(BasicBlock *bb) {
    Sdno.at(bb) = dfsCounter;
    Ndfs[dfsCounter] = bb;
    Label[bb] = bb;
    Ancestor[bb] = nullptr;
    dfsCounter++;
    for(BasicBlock *nbb : bb->getSuccs()) {
      if(Sdno.at(nbb) == 0) {
	Parent[nbb] = bb;
	DFS(nbb);
      }
    }
  }
  /* these methods are from Lengauer-Tarjan paper
   * and implement O(n*lg(n)) scheme */
  void Link(BasicBlock *v, BasicBlock *w) {
    Ancestor.at(w) = v;
  }
  void Compress(BasicBlock *bb) {
    if(Ancestor.at(Ancestor.at(bb))) {
      Compress(Ancestor.at(bb));
      if(Sdno.at(Label.at(Ancestor.at(bb))) < Sdno.at(Label.at(bb))) {
	Label.at(bb) = Label.at(Ancestor.at(bb));
      }
      Ancestor.at(bb) = Ancestor.at(Ancestor.at(bb));
    }
  }
  BasicBlock *Eval(BasicBlock *bb) {
    if(!Ancestor.at(bb)) {
      return bb;
    }
    else {
      Compress(bb);
      return Label.at(bb);
    }
  }
  
public:
  LengauerTarjanDominators(std::vector<BasicBlock*> &blocks) : blocks(blocks) {}
  void operator()() {
    for(BasicBlock *bb : blocks) {
      if(bb->getPreds().empty()) {
	assert(root==nullptr);
	root = bb;
      }
      Sdno[bb] = 0;
      Parent[bb] = nullptr;
      bb->getIdom() = nullptr;
    }
    assert(root);
    DFS(root);
    
    for(ssize_t i = (dfsCounter-1); i > 1; i--) {
      BasicBlock *w = Ndfs.at(i);
      ssize_t &sdom_w = Sdno.at(w);
      for(BasicBlock *pbb : w->getPreds()) {
	BasicBlock *u = Eval(pbb);
	ssize_t sdom_u = Sdno.at(u);
	if(sdom_u < sdom_w) {
	  sdom_w = sdom_u;
	}
      }
      Buckets[Ndfs.at(sdom_w)].insert(w);
      Link(Parent.at(w), w);
      /* need to understand - why parent bucket? */
      std::set<BasicBlock*> &pBucket = Buckets[Parent.at(w)];
      for(BasicBlock *v : pBucket) {
	/* find ancestor with lowest semidominator */
	BasicBlock *u = Eval(v);
	if(Sdno.at(u) < Sdno.at(v)) {
	  /* idom is the semidominator */
	  v->getIdom() = u;
	}
	else {
	  /* must defer */
	  v->getIdom() = Parent.at(w);
	}
      }
      pBucket.clear();
    }
    for(ssize_t i = 2; i < dfsCounter; i++) {
      BasicBlock *w = Ndfs.at(i);
      if(w->getIdom() != Ndfs.at(Sdno.at(w))) {
	w->getIdom() = w->getIdom()->getIdom();
      }
    }
    for(BasicBlock *bb : blocks) {
      if(bb->getPreds().empty()) {
	continue;
      }
      bb->getIdom()->addDTreeSucc(bb);
    }
  }
};


void computeDominance(std::vector<BasicBlock*> &basicBlocks){
  BasicBlock *entryBlock = nullptr;
  for(BasicBlock *bb : basicBlocks) {
    if(bb->preds.empty()) {
      entryBlock = bb;
      break;
    }
  }
  assert(entryBlock);

  LengauerTarjanDominators LTD(basicBlocks);
  LTD();
  
  entryBlock->idom = nullptr;

  /* compute dominance frontiers */
  for(auto cbb : basicBlocks) {
    /* only blocks with more than one pred
     * can update dominance frontiers */
    if(cbb->preds.size() > 1) {
      for(auto nbb : cbb->preds) {
	/* iterate back to idom 
	 * adding this node to 
	 * appropriate dominance frontiers */
	while((nbb != cbb->idom)) {
	    nbb->dfrontier.push_back(cbb);
	    nbb = nbb->idom;
	  }
      }
    }
  }
} 


CodeGenerator::CodeGenerator() {
  code = new List<Instruction*>();
}

CodeGenerator::~CodeGenerator() {
  if(code) {
    for(Instruction *insn : *code) {
      delete insn;
    }
    delete code;
  }
  for(Function *F : functionList) {
    delete F;
  }
}

//Number of automatic (and temporary variables)
int CodeGenerator::CodeSize() {
  return LocalAndTempCount; 
}

char *CodeGenerator::NewLabel()
{
  static int nextLabelNum = 0;
  
  //labels aren't going to overflow 2^(8*253)
  char temp[MAX_STR];
  
  snprintf(temp, MAX_STR, "_L%d", nextLabelNum++);
  return strdup(temp);
}

//generate a variable in the global data region and increment the global 
//pointer


//overloaded function, to print the var name, instead of generic _global
Location *CodeGenerator::GenGlobalVar(const char* str)
{

  char temp[MAX_STR];
  Location *result = NULL;
  int TempOffset = 0;

  snprintf(temp,MAX_STR,"_G%s", str);

  TempOffset = this->currentGlobal;

  result = new Location( gpRelative, TempOffset, temp); 

  assert(result != NULL);

  this->currentGlobal+=VarSize;

  globalVariableMap[std::string(result->GetName())] = new Variable(result);

  return result;
}


//overloaded version which generate with a name instead of generic _param
Location *CodeGenerator::GenParamVar(const char* str, int position)
{
  char temp[MAX_STR];
  Location *result = NULL;
  int TempOffset = 0;

  snprintf(temp,MAX_STR, "_P%s",str);

  TempOffset = position*4 + OffsetToFirstParam;

  result = new Location( fpRelative, TempOffset, temp); 
  result->isFuncArg = true;
  result->funcArgName = std::string(str);

  assert(result != NULL);

  return result;
}

Location *CodeGenerator::GenTempVar()
{
  static int nextTempNum;
  char temp[MAX_STR];
  Location *result = NULL;
  int TempOffset = 0;

  snprintf(temp,MAX_STR,"_tmp%d", nextTempNum++);

  TempOffset =  OffsetToFirstLocal - 4*LocalAndTempCount;
  LocalAndTempCount++;

  result = new Location(fpRelative, TempOffset, temp); 

  assert(result != NULL);
  return result;
}

//overloaded version whicih returns with a name instead of _temp
Location *CodeGenerator::GenTempVar(const char *str)
{
  static int nextTempNum;
  char temp[MAX_STR];
  Location *result = NULL;
  int TempOffset = 0;

  snprintf(temp, MAX_STR, "_T%s",str);

  TempOffset =  OffsetToFirstLocal - 4*LocalAndTempCount;
  LocalAndTempCount++;

  result = new Location(fpRelative, TempOffset, temp); 

  assert(result != NULL);
  return result;
}

Location *CodeGenerator::GenLoadConstant(int value)
{
  Location *result = GenTempVar();
  code->append(new LoadConstant(result, value));
  return result;
}

Location *CodeGenerator::GenLoadConstant(const char *s)
{
  Location *result = GenTempVar();
  code->append(new LoadStringConstant(result, s));
  return result;
}

Location *CodeGenerator::GenLoadLabel(const char *label)
{
  Location *result = GenTempVar();
  code->append(new LoadLabel(result, label));
  return result;
}


void CodeGenerator::GenAssign(Location *dst, Location *src)
{
  code->append(new Assign(dst, src));
}


Location *CodeGenerator::GenLoad(Location *ref, int offset)
{
  Location *result = GenTempVar();
  code->append(new Load(result, ref, offset));
  return result;
}

void CodeGenerator::GenStore(Location *dst,Location *src, int offset)
{
  code->append(new Store(dst, src, offset));
}


Location *CodeGenerator::GenBinaryOp(const char *opName, Location *op1,
						     Location *op2)
{
  Location *result = GenTempVar();
  if(enable_float && op1->GetFloat() && op2->GetFloat())
    result->SetFloat(true);

 
  code->append(new BinaryOp(BinaryOp::OpCodeForName(opName), result, op1, op2));
  return result;
}

//Single precision
Location *CodeGenerator::GenFloatCmp( Location *op1, Location *op2)
{
  Location *result = GenTempVar();
  assert(op1);
  assert(op2);

  code->append(new PushParam(op1));
  code->append(new PushParam(op2));

  code->append(new LCall("_CmpFloat", result));
  GenPopParams(2*VarSize);

  return result;
}

//Single precision
Location *CodeGenerator::GenFloatLe( Location *op1, Location *op2)
{
  Location *result = GenTempVar();
  assert(op1);
  assert(op2);

  code->append(new PushParam(op1));
  code->append(new PushParam(op2));

  code->append(new LCall("_LeFloat", result));
  GenPopParams(2*VarSize);

  return result;
}

void CodeGenerator::GenLabel(const char *label)
{
  code->append(new Label(label));
}

void CodeGenerator::GenIfZ(Location *test, const char *label)
{
  code->append(new IfZ(test, label));
}

void CodeGenerator::GenGoto(const char *label)
{
  code->append(new Goto(label));
}

void CodeGenerator::GenReturn(Location *val)
{
  code->append(new Return(val));
}


BeginFunc *CodeGenerator::GenBeginFunc()
{
  BeginFunc *result = new BeginFunc;
 
  //Reset automatic variable count in new function
  LocalAndTempCount = 0;

  code->append(result);
  return result;
}

void CodeGenerator::GenEndFunc()
{
  code->append(new EndFunc());
}

void CodeGenerator::GenPushParam(Location *param)
{
  code->append(new PushParam(param));
}

void CodeGenerator::GenPopParams(int numBytesOfParams)
{
  assert(numBytesOfParams >= 0 && numBytesOfParams % VarSize == 0); // sanity check
  if (numBytesOfParams > 0)
    code->append(new PopParams(numBytesOfParams));
}

Location *CodeGenerator::GenLCall(const char *label, bool fnHasReturnValue)
{
  Location *result = fnHasReturnValue ? GenTempVar() : NULL;
  code->append(new LCall(label, result));
  return result;
}

Location *CodeGenerator::GenACall(Location *fnAddr, bool fnHasReturnValue)
{
  Location *result = fnHasReturnValue ? GenTempVar() : NULL;
  code->append(new ACall(fnAddr, result));
  return result;
}


static struct _builtin {
  const char *label;
  int numArgs;
  bool hasReturn;
} builtins[] =
 {{"_Alloc", 1, true},
  {"_ReadLine", 0, true},
  {"_ReadInteger", 0, true},
  {"_StringEqual", 2, true},
  {"_PrintInt", 1, false},
  {"_PrintString", 1, false},
  {"_PrintBool", 1, false},
  {"_Halt", 0, false},
  {"_GetByte", 2, true},
  {"_SetByte", 3, true}
};



Location *CodeGenerator::GenBuiltInCall(BuiltIn bn,Location *arg1, Location *arg2)
{
  assert(bn >= 0);
  assert(bn < NumBuiltIns);
  struct _builtin *b = &builtins[bn];
  Location *result = NULL;

  if (b->hasReturn) result = GenTempVar();
                // verify appropriate number of non-NULL arguments given
  assert((b->numArgs == 0 && !arg1 && !arg2)
	|| (b->numArgs == 1 && arg1 && !arg2)
	|| (b->numArgs == 2 && arg1 && arg2));
  if (arg2) code->append(new PushParam(arg2));
  if (arg1) code->append(new PushParam(arg1));
  code->append(new LCall(b->label, result));
  GenPopParams(VarSize*b->numArgs);
  return result;
}
Location *CodeGenerator::GenSetByte(Location *address, 
				    Location *offset,
				    Location *value)
{
  Location *result = GenTempVar();
  assert(value);
  assert(offset);
  assert(value);

  code->append(new PushParam(value));
  code->append(new PushParam(offset));
  code->append(new PushParam(address)); 
  code->append(new LCall("_SetByte", result));
  GenPopParams(3*VarSize);
  return result;
}

Location *CodeGenerator::GenPrintFloat(Location *value)
{
  Location *result = 0;
  assert(value);

  code->append(new PushParam(value));
  code->append(new LCall("_PrintFloat", result));
  GenPopParams(VarSize);


  return result;
}


void CodeGenerator::GenVTable(const char *className, List<const char *> *methodLabels)
{
  code->append(new VTable(className, methodLabels));
}

void BasicBlock::PrintSSA() {
  printf("Block %s: \n", name.c_str());
  PrintPhis();
  for(Instruction* insn: ins) {
    insn->PrintSSA();
  }
  printf("....\n");
}

void BasicBlock::PrintPhis() {
  for(rVariable *rv :uphis) {
    printf("%s : ", rv->getName().c_str());
    for(std::map<BasicBlock*, rVariable*>::iterator mit = rv->phiEdges.begin();
	mit != rv->phiEdges.end(); mit++)
      {
	  if(mit->second)
	    {
	      printf("(%s,%s)", 
		     mit->first->name.c_str(),
		     mit->second->getName().c_str());
	    }
	  else
	    {
	      printf("(%s,NULL)", 
		     mit->first->name.c_str());

	    }
	}
      printf("\n");
    }
}

bool BasicBlock::addPhi(Variable *v)
{
  if(phis.find(v) != phis.end())
    return false;
  phis.insert(v);
  v->addAssign(this);
  return true;
}

BasicBlock* BasicBlock::split(std::vector<BasicBlock*> &newBlocks)
{
  if(ins.size()==0)
    return 0;

  std::vector<size_t> splits; 
  std::vector<BasicBlock*> blks;
  blks.push_back(this);

  int nGotos = 0;
  int nIfZs = 0;

  for(size_t i = 0; i < (ins.size()-1); i++)
    {
      Instruction *insn = ins[i];
      
      if(dynamic_cast<IfZ*>(insn))
	{
	  splits.push_back(i);
	  nIfZs++;
	}
      else if(dynamic_cast<Goto*>(insn))
	{
	  splits.push_back(i);
	  nGotos++;
	}
    }

  if(splits.size() > 0) 
    {
      //printf("name=%s, splits=%zu\n", name.c_str(), splits.size());
      for(size_t i = 1; i <= splits.size(); i++)
	{
	  size_t start = splits[i-1]+1;
	  size_t stop = (i==splits.size()) ? (ins.size()-1) : splits[i];
	  BasicBlock *bb = new BasicBlock(this->name + "_" + std::to_string(i));
	  bb->parentFunc = this->parentFunc;
	  blks.push_back(bb);
	  for(size_t j = start; j <= stop; j++) {
	    bb->ins.push_back(ins[j]);
	  }
	  /*bb->Print();*/
	}

      ins.erase(ins.begin()+splits[0]+1, ins.end());

      for(size_t i = 0; i < (blks.size()-1); i++)
	{
	  if(blks[i]->hasReturn())
	    continue;
	  if(blks[i]->hasGoto())
	    continue;
	  blks[i]->add_succ(blks[i+1]);
	}
      for(size_t i = 1; i < blks.size(); i++)
	{
	  //blks[i]->Print();
	  newBlocks.push_back(blks[i]);
	}
      return blks[blks.size()-1];
    } 
  else 
    {
      return 0;
    }

  //if(splits.size() > 0) {
  //printf("splits=%zu\n", splits.size());
  //exit(-1);
  //}
}
void BasicBlock::add_links(std::map<std::string, BasicBlock*> &blockMap)
{
  for(size_t i = 0; i < ins.size(); i++)
    {
      Instruction *insn = ins[i];
      IfZ * insn_ifz = dynamic_cast<IfZ*>(insn);
      Goto * insn_goto = dynamic_cast<Goto*>(insn);
      if(insn_ifz) 
	{
	  std::string next = insn_ifz->getLabel();
	  BasicBlock *bb = blockMap[next];
	  takenBlock = bb;
	  assert(bb);
	  this->add_succ(bb);
	}
      else if(insn_goto)
	{
	  std::string next = insn_goto->getLabel();
	  BasicBlock *bb = blockMap[next];
	  takenBlock = bb;
	  assert(bb);
	  this->add_succ(bb);
	}
    }
}



void BasicBlock::generateLLVM(llvm::IRBuilder<> *myIRBuilder, 
			      llvm::LLVMContext *Context,
			      llvm::Module *Module) {
  bool hasReturn = false;
  llvm::Type *iPtr32 = llvm::Type::getInt32PtrTy(*Context);
  llvm::Type *iPtr8 = llvm::Type::getInt8PtrTy(*Context);
  llvm::Type *i32 = llvm::Type::getInt32Ty(*Context);
  llvm::Value *vBogoReturn = llvm::ConstantInt::get(i32,-1);
  llvm::Value *vZero = llvm::ConstantInt::get(i32, 0);

  std::vector<llvm::Value*> argVector;
  myZeroValue = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Context),
				       0);
  
  for(auto rv : uphis){
    rv->myValue = myIRBuilder->CreatePHI(llvm::Type::getInt32Ty(*Context),
					 0, rv->getName().c_str());
  }

  for(auto insn : ins) {
    const Instruction::TACTypes tacType = insn->getType();
    switch(tacType)
      {
	case Instruction::LoadConstant:
	  {
	    LoadConstant *LC = dynamic_cast<LoadConstant*>(insn);
	    assert(LC->rDst);
	    LC->rDst->myValue = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*Context),
						      LC->val);
	    break;
	  }
	case Instruction::LoadStringConstant:
	  {
	    LoadStringConstant *LSC = dynamic_cast<LoadStringConstant*>(insn);

	    char *tbuf = 0;
	    size_t src=0,dst=0,len;
	    llvm::Value *vStr = 0;

	    len=strlen(LSC->str);
	    tbuf = new char[len+1];
	    strcpy(tbuf,LSC->str);
	    
	    while(src < len)
	      {
		/* remove initial quotes */
		if(src == 0 || src == (len-1))
		  {
		    src++;
		  }
		/* generate proper newlines */
		else if(tbuf[src+0] == '\\' && tbuf[src+1] == 'n' && (src < (len-1)))
		  {
		    tbuf[dst++] = '\n';
		    src += 2;
		  }
		/* generate proper tab characters */
		else if(tbuf[src+0] == '\\' && tbuf[src+1] == 't' && (src < (len-1)))
		  {	
		    tbuf[dst++] = '\t';
		    src += 2;
		  }
		/* copy string normally */
		else
		  {
		    tbuf[dst++] = tbuf[src++];
		  }
	      }
	    tbuf[dst] = '\0';
	    std::string s = std::string(tbuf);

	    if(globalStringMap.find(s) != globalStringMap.end())
	      {
		vStr = globalStringMap[s];
	      }
	    else
	      {
		vStr = myIRBuilder->CreateGlobalStringPtr(s.c_str());
		globalStringMap[s] = vStr;
	      }
	    LSC->rDst->myValue = myIRBuilder->CreatePtrToInt(vStr, i32);
	    delete [] tbuf;
	    break;
	  }
	case Instruction::LoadLabel:
	  {
	    LoadLabel *LL = dynamic_cast<LoadLabel*>(insn);
	    if(!LL->rDst){ printf("%d\n", __LINE__); exit(-1); }
	    
	    //LL->rDst->myValue = myZeroValue;
	    //printf("load label %s!!!!\n", LL->label);
	    if(labelMap.find(LL->label)==labelMap.end())
	      {
		printf("couldn't find label!!\n");
		exit(-1);
	      }
	    LL->rDst->myValue = myIRBuilder->CreatePtrToInt(labelMap[LL->label], i32);
	    //exit(-1);
	    break;
	  }
	case Instruction::Assign:
	  {
	    Assign *A = dynamic_cast<Assign*>(insn);

	    if(!A->rSrc) 
	      { printf("%d\n", __LINE__); abort(); }
	    if(!A->rDst) 
	      {
		printf("%s\n", A->dst->GetName());
		printf("%d\n", __LINE__); 
		exit(-1);
		//gV = A->rSrc->myValue;
	      }
	   
	    A->rDst->setLLVMValue(myIRBuilder, Context, 
				  A->rSrc->getLLVMValue(myIRBuilder,Context));
	    break;
	  }
	case Instruction::Load:
	  {
	    Load *L = dynamic_cast<Load*>(insn);
	    //L->Print();

	    //src is the address
	    //dst is the value
	    // printf("address=%s (%p), value=%s (%p) offset = %d\n", 
		   //	   L->rSrc->getName().c_str(), 
		   //	   L->rSrc,
		   //	   L->rDst->getName().c_str(),
		   //	   L->rDst,
		   //	   L->offset);
	    //printf("need to generate a load...\n"); abort();
	    if(!L->rSrc) abort();
	    if(!L->rDst) abort();
	    if(!L->rSrc->myValue) 
	      { printf("can't find address : %d\n", __LINE__); abort(); }
	    //if(L->offset != 0) 
	    //{ printf("non zero offset in store\n"); abort(); }

	    llvm::Value *vAddr = L->rSrc->getLLVMValue(myIRBuilder,Context);
	    llvm::Value *vBPtr = myIRBuilder->CreateIntToPtr(vAddr, iPtr8);
	    llvm::Value *vOffset = llvm::ConstantInt::get(i32,L->offset);
	    llvm::Value *vGEP = myIRBuilder->CreateGEP(vBPtr, vOffset);
	    vGEP = myIRBuilder->CreateBitCast(vGEP, iPtr32);
	    llvm::Value *vLoad = myIRBuilder->CreateLoad(vGEP);
	    L->rDst->setLLVMValue(myIRBuilder, Context, vLoad);
	    
	    break;
	  }
	case Instruction::Store:
	  {
	    Store *S = dynamic_cast<Store*>(insn);

	    //S->Print();
	    if(!S->rSrc) 
	      { printf("%d\n", __LINE__); abort(); }
	    if(!S->rDst) 
	      { printf("%d\n", __LINE__); abort(); }


	    //printf("value=%s (%p), address=%s (%p) offset = %d\n", 
	    //S->rSrc->getName().c_str(), 
	    //	   S->rSrc->myValue,
	    //S->rDst->getName().c_str(),
	    //S->rDst->myValue,
	    //	   S->offset);

	    if(!S->rSrc->myValue) 
	      { printf("can't find value : %d\n", __LINE__); abort(); }
	    if(!S->rDst->myValue) 
	      { printf("can't find address : %d\n", __LINE__); abort(); }

	  
	    llvm::Value *vBPtr=  myIRBuilder->CreateIntToPtr(S->rDst->myValue, iPtr8);
	    llvm::Value *vOffset = llvm::ConstantInt::get(i32,S->offset);

	    llvm::Value *vGEP = myIRBuilder->CreateGEP(vBPtr, vOffset);
	    vGEP = myIRBuilder->CreateBitCast(vGEP, iPtr32);
	    myIRBuilder->CreateStore(S->rSrc->myValue, vGEP);
	    
	    if(S->offset != 0) 
	      {
		printf("non zero offset in store\n");
		abort();
	      }
	    

	    //printf("need to generate a store...\n"); abort();
	    break;
	  }
	case Instruction::BinaryOp:
	  {
	    BinaryOp *B = dynamic_cast<BinaryOp*>(insn);
	    llvm::Value *v1,*v2;

	    if(!B->rOp1) 
	      abort();
	    if(!B->rOp2) 
	      abort();
	    if(!B->rOp1->myValue) 
	      {
		printf("couldnt find %s\n", B->rOp1->getName().c_str());
		abort();
	      }
	    if(!B->rOp2->myValue) 
	      {
		printf("couldnt find %s\n", B->rOp2->getName().c_str());
		abort();
	      }

	    v1 = B->rOp1->getLLVMValue(myIRBuilder, Context);
	    v2 = B->rOp2->getLLVMValue(myIRBuilder, Context);

	    if(!B->rDst) 
	      abort();
	    switch(B->code)
	      {
	      case BinaryOp::Add:
		B->rDst->myValue = myIRBuilder->CreateAdd(v1,v2); 
		break;
	      case BinaryOp::Sub:
		B->rDst->myValue =  myIRBuilder->CreateSub(v1,v2);
		break;
	      case BinaryOp::Mul:
		B->rDst->myValue = myIRBuilder->CreateMul(v1,v2);
		break;
	      case BinaryOp::Div:
		B->rDst->myValue = myIRBuilder->CreateSDiv(v1,v2);
		break;
	      case BinaryOp::Mod:
		B->rDst->myValue = myIRBuilder->CreateSRem(v1,v2);
		break;
	      case BinaryOp::Eq:
		{
		  llvm::Value *vCmp = myIRBuilder->CreateICmpEQ(v1,v2);
		  B->rDst->myValue =  myIRBuilder->CreateZExt(vCmp, i32);
		  break;
		}
	      case BinaryOp::Less:
		{
		  llvm::Value *vCmp = myIRBuilder->CreateICmpSLT(v1,v2);
		  B->rDst->myValue =  myIRBuilder->CreateZExt(vCmp, i32);
		  break;
		}
	      case BinaryOp::And:
		B->rDst->myValue = myIRBuilder->CreateAnd(v1,v2);
		break;
	      case BinaryOp::Or:
		B->rDst->myValue = myIRBuilder->CreateOr(v1,v2);
		break;
	      default:
		abort();
		break;
	      }
	    break;
	  }
	case Instruction::Label:
	  break;
	  /*
	case Instruction::Goto:
	  break;
	case Instruction::IfZ:
	  {
	    IfZ *Z = dynamic_cast<IfZ*>(insn);
	    //Variable *v = variableMap[Z->test];
	    //Z->rTest = bb->rMap[v];
	    break;
	  }
	  */
	case Instruction::BeginFunc:
	  break;
	case Instruction::EndFunc:
	  break;
	case Instruction::Return:
	  {
	    hasReturn = true;
	    Return *R = dynamic_cast<Return*>(insn);
	    if(R->val) 
	      {
		bool nullReturn = (R->rVal==0);
		if(!nullReturn)
		  {
		    if(!R->rVal->myValue) 
		      abort();
		    myIRBuilder->CreateRet(R->rVal->getLLVMValue(myIRBuilder,Context));
		  }
		else
		  myIRBuilder->CreateRet(vBogoReturn);
	      } 
	    else 
	      {
		abort();
	      }
	    break;
	  }
	case Instruction::PushParam:
	  {
	    PushParam *PP = dynamic_cast<PushParam*>(insn);
	    if(!PP->rParam) { 
	      printf("%d\n", __LINE__); exit(-1); 
	    }
	    argVector.push_back(PP->rParam->getLLVMValue(myIRBuilder,Context));
	    break;
	  }
	case Instruction::PopParams:
	  argVector.clear();
	  break;
	case Instruction::LCall:
	  {
	    LCall *L = dynamic_cast<LCall*>(insn);
	    //stack sematics
	    std::reverse(argVector.begin(), argVector.end());
	    llvm::ArrayRef<llvm::Value*> cArgs(argVector);

	    if(_builtinFuncs.find(std::string(L->label)) 
	       != _builtinFuncs.end())
	      {
		//printf("calling func %s with %zu arguments\n",
		//      L->label, argVector.size());
		llvm::Value *v = myIRBuilder->CreateCall(_builtinFuncs[L->label],
							 cArgs);
		if(L->dst)
		  {
		    //printf("setting dst value to %s\n", L->rDst->getName().c_str());
		    L->rDst->myValue = v;
		  }
	      } 
	    else 
	      {
		llvm::Function *FC = Module->getFunction(L->label);
		if(!FC) {
		  printf("couldn't find function %s!\n", L->label);
		  exit(-1);
		} 

		llvm::Value *r = myIRBuilder->CreateCall(FC, cArgs);
		//L->rDst->myValue = myZeroValue;
		if(L->dst)
		  {
		    L->rDst->myValue = r;
		  }
	      }

	    break;
	  }
	case Instruction::ACall:
	  {
	    ACall *A = dynamic_cast<ACall*>(insn);
	    std::reverse(argVector.begin(), argVector.end());
	    llvm::ArrayRef<llvm::Value*> cArgs(argVector);

	    //generate required type 
	    llvm::FunctionType *fptrType = 0;
	    std::vector<llvm::Type*> argTypes;
	    llvm::Type *i32 = llvm::Type::getInt32Ty(*Context);

	    for(size_t i = 0; i < argVector.size(); i++)
	      {
		argTypes.push_back(i32);
	      }
	    llvm::ArrayRef<llvm::Type*> blockArgs(argTypes);
	    llvm::FunctionType *fpType =  llvm::FunctionType::get(i32, blockArgs, false);
	    if(!fpType) {
	      printf("couldn't get function pointer type\n");
	      exit(-1);
	    }

	    if(!A->rMethodAddr) {
	      printf("no method address\n");
	      exit(-1);
	    }
	    if(!A->rMethodAddr->myValue) {
	      printf("no method llvm value\n");
	      exit(-1);
	    }
	    llvm::Value *vAddr = A->rMethodAddr->getLLVMValue(myIRBuilder,Context);

	    llvm::Value *vFptr=  myIRBuilder->CreateIntToPtr(vAddr,
							     fpType->getPointerTo(),
							     "methodCallCast");
	    if(!vFptr) {
	      printf("no function pointer...\n");
	      exit(-1);
	    }

	    llvm::Value *r = myIRBuilder->CreateCall(vFptr, cArgs);
	    if(A->dst)
	      {
		A->rDst->setLLVMValue(myIRBuilder,Context,r);
	      }
	    break;
	  }
	case Instruction::VTable:
	  break;
	default:
	  break;
	}
    }


  if(!hasReturn)
    {
      if(ins.size() > 0)
	{
	  Instruction *insn = ins[ins.size()-1];
	  const Instruction::TACTypes tacType = insn->getType();
	  //printf("name=%s, tacType=%d\n", name.c_str(), (int)tacType);
	  switch(tacType)
	    {
	    case Instruction::Goto:
	      {
		if(succs.size() != 1 || takenBlock == 0)
		  {
		    printf("succs.size()=%zu, takenBlock=%p\n", 
			   succs.size(), takenBlock);
		    printf("name=%s, tacType=%d\n", name.c_str(), (int)tacType);
		    Print();
		    exit(-1);
		  }

		llvm::Value *v = myIRBuilder->CreateBr(takenBlock->myBlock);
		//v->dump(); fflush(NULL);
		break;
	      }
	    case Instruction::IfZ:
	      {
		IfZ *Z = dynamic_cast<IfZ*>(insn);

		if(succs.size() != 2 || takenBlock == 0) {
		  printf("IfZ has %zu succs\n", succs.size());
		  exit(-1);
		}

		llvm::Value *vCmp = myIRBuilder->CreateICmpEQ(Z->rTest->myValue, vZero);

		llvm::BasicBlock *tBB = 0, *fBB = 0;
		for(std::set<BasicBlock*>::iterator it = succs.begin();
		    it != succs.end(); it++)
		  {
		    BasicBlock *NBB = *it;
		    if(NBB == takenBlock)
		      {
			tBB = NBB->myBlock;
		      }
		    else
		      {
			fBB = NBB->myBlock;
		      }
		  }
		myIRBuilder->CreateCondBr(vCmp, tBB, fBB);

		break;
	      }
	    default:
	      {
		if(hasReturn && succs.size() != 0) {
		  printf("return with successors!\n");
		  exit(-1);
		}

		if(succs.size()==0)
		  {
		    /* Todo : call exit */
		    if(!hasReturn)
		      {
			myIRBuilder->CreateRet(vBogoReturn);
		      }
		  }
		else
		  {
		    //insn->Print();
		    std::set<BasicBlock*>::iterator sit = succs.begin();
		    BasicBlock *n = *sit;
		    myIRBuilder->CreateBr(n->myBlock);
		    break;
		  }
	      }
	    }
	}
      else
	{
	  assert(succs.size() == 1);
	  std::set<BasicBlock*>::iterator sit = succs.begin();
	  BasicBlock *n = *sit;
	  assert(n->myBlock);
	  myIRBuilder->CreateBr(n->myBlock);
	}
    }


  for(auto BB : dtree_succs) {
    myIRBuilder->SetInsertPoint(BB->myBlock);
    BB->generateLLVM(myIRBuilder,Context,Module);
  }
 
 
}

void BasicBlock::fixPHIs(llvm::IRBuilder<> *myIRBuilder, 
			 llvm::LLVMContext *Context,
			 llvm::Module *Module)
{
  //patch up phis
  for(auto rv : uphis) {
    llvm::PHINode *pN = llvm::dyn_cast<llvm::PHINode>(rv->myValue);
      if(!pN) 
	abort();
      for(std::map<BasicBlock*, rVariable*>::iterator mit = rv->phiEdges.begin();
	  mit != rv->phiEdges.end(); mit++)
	{
	  BasicBlock *BB = mit->first;
	  if(!BB) 
	    abort();
	  llvm::BasicBlock *PBB = BB->myBlock;
	  if(!PBB)
	    abort();
	  if(mit->second)
	    {
	      if(!mit->second->myValue) {
		printf("missing value for %s!\n", mit->second->getName().c_str());
		abort();
	      }
	       
	      pN->addIncoming(mit->second->myValue, PBB);
	    }
	  else
	    {
	      pN->addIncoming(BB->myZeroValue, PBB);
	    }
	}
            
    }

  for(std::set<BasicBlock*>::iterator it = dtree_succs.begin(); 
      it != dtree_succs.end(); it++)
     {
       BasicBlock *BB = *it;
       myIRBuilder->SetInsertPoint(BB->myBlock);
       BB->fixPHIs(myIRBuilder,Context,Module);
     }
}




void Function::generateLLVM(llvm::IRBuilder<> *myIRBuilder, 
			    llvm::LLVMContext *Context,
			    llvm::Module *Module) {

  for(BasicBlock *BB : blocks){
    BB->myBlock = llvm::BasicBlock::Create(*Context, BB->name.c_str(), this->myFunction);
  }

  myIRBuilder->SetInsertPoint(entry->myBlock);
  std::map<std::string, llvm::Value*> argMap;
  size_t idx = 0;
  for (llvm::Function::arg_iterator AI = myFunction->arg_begin(); 
       AI != myFunction->arg_end(); ++AI) {
      llvm::Value *a = &*AI;
      std::string aN = argNames[idx++];
      //printf("aN=%s\n", aN.c_str());
      argMap[aN] = a;
    }

  //patch up vtables in main
  if(name == "main") {
    llvm::Type *iPtr32 = llvm::Type::getInt32PtrTy(*Context);
    llvm::Type *i32 = llvm::Type::getInt32Ty(*Context);
    
    for(std::map<std::string, std::vector<std::string> >::iterator mit = 
	  vtableNames.begin(); mit != vtableNames.end(); mit++)
	{
	  std::string className = mit->first;
	  llvm::Value *vTable = labelMap[className];
	  llvm::Value *vTbl = myIRBuilder->CreateBitCast(vTable, iPtr32);
	  size_t idx = 0;

	  for(std::vector<std::string>::iterator vit = mit->second.begin();
	      vit != mit->second.end(); vit++)
	    {
	      std::string s = *vit;
	      llvm::Function *FC = Module->getFunction(s.c_str());
	      if(!FC) {
		printf("couldn't find %s\n",  s.c_str());
		exit(-1);
	      }
	      llvm::Value *vOffset = llvm::ConstantInt::get(i32,idx);
	      llvm::Value *vGEP = myIRBuilder->CreateGEP(vTbl, vOffset);
	      
	      
	      llvm::Value *vFptr = myIRBuilder->CreatePtrToInt(FC, i32);
	      //vFptr = vOffset;

	      myIRBuilder->CreateStore(vFptr, vGEP);
	      idx++;
	    }
	  //call runtime init function 
	  myIRBuilder->CreateCall(_builtinFuncs["_RuntimeInit"]);
	}
      //exit(-1);
    }


  //if(argVector.size() != entry->funcArgs.size()) {
  //    printf("function argument mismatch, should be %zu but got %zu!\n",
  //argVector.size(), entry->funcArgs.size());
  //    abort();
  //}


  for(std::set<rVariable*>::iterator it = entry->funcArgs.begin();
      it != entry->funcArgs.end(); it++)
    {
      rVariable *rv = *it;
      std::string aN = rv->getName();
      //printf("rv = %s\n", rv->getName().c_str());
      if(argMap.find(aN) == argMap.end())
	{
	  printf("unable to find argument %s\n", aN.c_str());
	  abort();
	}
      rv->myValue = argMap[aN];
    }
  entry->generateLLVM(myIRBuilder,Context,Module);

  myIRBuilder->SetInsertPoint(entry->myBlock);
  entry->fixPHIs(myIRBuilder,Context,Module);

}

void Function::asDot(std::string fname) {}

void BasicBlock::naturalLoopDFS() {
  color = 1;

  auto dominates = [](const BasicBlock *A, const BasicBlock *B) {
    if(A==B)
      return true;
    if(B->idom == A)
      return true;
    if(A->idom == B)
      return false;
    
    const BasicBlock *P = B->idom;
    while(true) {
      if(P==A)
	return true;
      else if(P->preds.empty())
	break;
      else
	P = P->idom;
    }
    return false;
  };
  
  for(BasicBlock *nb : succs) {
    if(nb->color == 1){
      if(dominates(nb, this)) {
	/* The natural loop of a back edge is
	 * the smallest set of nodes that includes
	 * the head and tail of the back edge, and
	 * has no predecessors outside the set,
	 * except for the predessors of the header */
	std::set<BasicBlock*> naturalLoop;
	std::list<BasicBlock*> queue;
	BasicBlock *header = nb;
	BasicBlock *latch = this;
	queue.push_back(latch);
	/* travere graph from tail to 
	 * head adding predecessor nodes */
	while(!queue.empty())
	  {
	    BasicBlock *b = queue.front();
	    queue.pop_front();
	    if(b==header)
	      continue;
	    if(naturalLoop.find(b) == naturalLoop.end())
	      {
		naturalLoop.insert(b);
		for(std::set<BasicBlock*>::iterator i = b->preds.begin();
		    i != b->preds.end(); i++)
		  {
		    queue.push_back(*i);
		  }
	      }
	  }
	printf("found loop with header %s that has %zu blocks!!!\n",
	       header->name.c_str(), naturalLoop.size());
      }
    }
    if(nb->color == 0)
      nb->naturalLoopDFS();
  }
  
  color = 2;
}


Function *CodeGenerator::getFunction(int &idx) {
  std::set<Location*> allLocs,spLocs,gblLocs;
  std::map<BasicBlock*, BasicBlock*> fallThroughBlock;
  bool removeDead = true;
  std::vector<BasicBlock*> newBlocks;
  
  if(idx < 0) { abort(); }
  Label *l = dynamic_cast<Label*>(code->get(idx-1));
  if(!l) { abort(); }
  Function *f = new Function(std::string(l->getLabel()));
  functionList.push_back(f);
  BeginFunc *bF = dynamic_cast<BeginFunc*>(code->get(idx));
  if(!bF) { abort(); }

  f->funcDecl = bF;

  /* we're now in the first basic block */
  BasicBlock *b = new BasicBlock(l->getLabel());
  b->parentFunc = f;
  f->blocks.push_back(b);
  f->blockMap[l->getLabel()] = b;

  f->entry = b;
  f->entry->isEntryBlock = true;

  /*idx++;*/
  while(true)
    {
      Instruction *ins = code->get(idx);
      //ins->Print();
      Label *l = dynamic_cast<Label*>(ins);
      EndFunc *e = dynamic_cast<EndFunc*>(ins);
      if(l)
	{
	  BasicBlock *bb = new BasicBlock(l->getLabel());
	  bb->parentFunc = f;
	  fallThroughBlock[b] = bb;
	  b = bb;
	  f->blocks.push_back(b);
	  f->blockMap[l->getLabel()] = b;
	}
      else if(e)
	{
	  b->ins.push_back(ins);
	  break;
	}
      else
	{
	  b->ins.push_back(ins);
	}
      idx++;
    }

  /* blocks are currently single entry, multiple exit..fix it! */
  for(std::vector<BasicBlock*>::iterator it = f->blocks.begin();
      it != f->blocks.end(); it++)
    {
      BasicBlock *b = *it;
      BasicBlock *lbb = b->split(newBlocks);

      if(lbb) {
	BasicBlock *f = fallThroughBlock[b];
	fallThroughBlock.erase(b);
	fallThroughBlock[lbb] = f;
      }
    }
  for(std::vector<BasicBlock*>::iterator it = newBlocks.begin();
      it != newBlocks.end(); it++)
    {
      /*printf("Adding block!\n");*/
      f->blocks.push_back(*it);
    }
  for(std::map<BasicBlock*, BasicBlock*>::iterator mit = fallThroughBlock.begin();
      mit != fallThroughBlock.end(); mit++)
    {
      BasicBlock *cbb = mit->first;
      if(cbb->hasTermGoto())
	continue;
      if(cbb->hasTermHalt()) {
	continue;
      }
      if(cbb->hasGoto())
	continue;

      BasicBlock *nbb = mit->second;
      if(!nbb) { printf("%d\n", __LINE__); exit(-1); }
      
      cbb->add_succ(nbb);
    }

  for(std::vector<BasicBlock*>::iterator it = f->blocks.begin();
      it != f->blocks.end(); it++)
    {
      BasicBlock *b = *it;
      b->add_links(f->blockMap);
    }

  //f->asDot(l->getLabel() + std::string(".dot"));
   
  for(std::vector<BasicBlock*>::iterator it = f->blocks.begin();
      it != f->blocks.end(); it++)
    {
      BasicBlock *b = *it;
      if(!b->hasValidSucc()) {
	printf("block %s does not have valid successors\n", b->name.c_str());
	b->Print();
      }
    }


  while(removeDead)
    {
      removeDead = false;
      for(std::vector<BasicBlock*>::iterator it = f->blocks.begin();
	  it != f->blocks.end(); it++)
	{
	  BasicBlock *b = *it;
	  if(b==f->entry)
	    continue;

	  if(b->preds.size() == 0) 
	    {
	      //printf("%s has no preds!\n", b->name.c_str());
	      /* remove all references to block */
	      for(std::set<BasicBlock*>::iterator vit = b->succs.begin();
		  vit != b->succs.end(); vit++)
		{
		  BasicBlock *bb = *vit;
		  std::set<BasicBlock*>::iterator sit = bb->preds.find(b);
		  bb->preds.erase(sit);
		}
	      f->blocks.erase(it);
	      removeDead = true;
	      //exit(-1);
	      break;
	    }
	}
    }

  
  for(std::vector<BasicBlock*>::iterator it0 = f->blocks.begin();
      it0 != f->blocks.end(); it0++)
    {
      BasicBlock *b = *it0;
      for(std::vector<Instruction*>::iterator it1 = b->ins.begin(); 
	  it1 != b->ins.end(); it1++)
	{
	  Instruction *insn = *it1;
	  insn->getLocations(allLocs);
	}
    }

  
  //if(!globalVariableValues.empty())
  //{
  //printf("global variables are not supported...\n");
  //exit(-1);
  //}


  for(std::set<Location*>::iterator it = allLocs.begin(); it != allLocs.end(); it++)
    {
      Location *l = *it;
      if(!l)
	continue;
      
      if(l->GetSegment() == fpRelative)  
	{
	  Variable *v = new Variable(l);
	  assert(l);
	  f->variableMap[l] = v;
	  f->variables.push_back(v);
	  spLocs.insert(l);
	}
      else
	{
	  //printf("found global variable %s\n", l->GetName());
	  gblLocs.insert(l);
	}
    }


  for(std::vector<BasicBlock*>::iterator it0 = f->blocks.begin();
      it0 != f->blocks.end(); it0++)
    {
      BasicBlock *b = *it0;
      for(std::vector<Instruction*>::iterator it1 = b->ins.begin(); 
	  it1 != b->ins.end(); it1++)
	{
	  Instruction *insn = *it1;
	  Location *dst = insn->getDst();
	  Variable *v = f->variableMap[dst];
	  if(dst && v) {
	    v->addAssign(b);
	    b->assigns.push_back(v);
	  }
	}
    }

  
  //printf("dominance computed\n");
  computeDominance(f->blocks);
  for(std::vector<BasicBlock*>::iterator it0 = f->blocks.begin();
      it0 != f->blocks.end(); it0++)
    {
      BasicBlock *b = *it0;
      b->clearColor();
    }
  //printf("function %s\n", f->name.c_str());
  //f->entry->naturalLoopDFS();

  //printf("computed phi locations\n");
  insertPhis(f->blocks,f->variables);
  //printf("inserted phis\n");
  reNameVariables(f->blocks,f->variables,f->variableMap);
  //printf("renamed variables\n");
  
  return f;
}


#if ((LLVM_VERSION_MAJOR==3 && LLVM_VERSION_MINOR > 8) || (LLVM_VERSION_MAJOR > 3))
llvm::LLVMContext globalContext;
#endif

void CodeGenerator::DoFinalCodeGen(std::string &srcName) {
  //Mips* mips = new Mips();
  //std::string fname = "decaf.asm";
  std::set<ClassDecl*> classes;
  std::vector<Function*> funcs;
  llvm::LLVMContext *Context = &globalContext;
  llvm::IRBuilder<> *myIRBuilder = new llvm::IRBuilder<>(*Context);
  llvm::Module *myModule = new llvm::Module("__decaf__", *Context);
  llvm::Type *iType32 = myIRBuilder->getInt32Ty();
  llvm::Type *vType = myIRBuilder->getVoidTy();
  
  //mips->EmitPreamble();
  //for (int i = 0; i < code->size(); i++) {
  // code->get(i)->Emit(mips);
  //}
  //mips->SaveMips(fname);
  //delete mips;

  //std::cout << "myModule = " << myModule << "\n";
  assert(myModule);
  
  for(auto & p : globalVariableMap) {
    const std::string &gVarName = p.first;
    auto gV =  new llvm::GlobalVariable(*myModule, iType32,false,
					llvm::GlobalValue::PrivateLinkage,
					llvm::Constant::getNullValue(iType32),
					gVarName);
    //mit->second = gV;
    rVariable *rv = new rVariable(0, p.second);
    rv->myValue = gV;
    globalVariableValues[gVarName] = rv;
  }

  for (int i = 0; i < code->size(); i++) {
    //code->get(i)->Print();
    //continue;
    BeginFunc *bF = dynamic_cast<BeginFunc*>(code->get(i));
    if(bF) {
      Function *F = getFunction(i);
      funcs.push_back(F);
      EndFunc *e = dynamic_cast<EndFunc*>(code->get(i));
      assert(e);
    }      
  }
  /* */
  auto VFT = llvm::FunctionType::get(vType,false);
  
  llvm::Constant *runtimeInitFunc = myModule->getOrInsertFunction("_RuntimeInit", VFT);
  _builtinFuncs["_RuntimeInit"] = llvm::cast<llvm::Function>(runtimeInitFunc);
  
  llvm::Constant *allocFunc = myModule->getOrInsertFunction("_Alloc", llvm::FunctionType::get(iType32, iType32,false));
  _builtinFuncs["_Alloc"] = llvm::cast<llvm::Function>(allocFunc);

  llvm::Constant *printStringFunc = myModule->getOrInsertFunction("_PrintString", 
								  llvm::FunctionType::get(vType,iType32, false));

  _builtinFuncs["_PrintString"] = llvm::cast<llvm::Function>(printStringFunc);


  llvm::Constant *ReadIntegerFunc = myModule->getOrInsertFunction("_ReadInteger",
								  llvm::FunctionType::get(iType32, false));
  _builtinFuncs["_ReadInteger"] = llvm::cast<llvm::Function>(ReadIntegerFunc);

  llvm::Constant *printIntFunc = myModule->getOrInsertFunction("_PrintInt",
							       llvm::FunctionType::get(vType,iType32, false));
  _builtinFuncs["_PrintInt"] = llvm::cast<llvm::Function>(printIntFunc);


  llvm::Constant *printBoolFunc = myModule->getOrInsertFunction("_PrintBool",
								llvm::FunctionType::get(vType,iType32, false));
  _builtinFuncs["_PrintBool"] = llvm::cast<llvm::Function>(printBoolFunc);


  llvm::Constant *stringEqualFunc = myModule->getOrInsertFunction("_StringEqual",
								  llvm::FunctionType::get(iType32,{iType32, iType32}, false));
  _builtinFuncs["_StringEqual"] = llvm::cast<llvm::Function>(stringEqualFunc);


  llvm::Constant *readLineFunc = myModule->getOrInsertFunction("_ReadLine",
							       llvm::FunctionType::get(iType32, false));
  _builtinFuncs["_ReadLine"] = llvm::cast<llvm::Function>(readLineFunc);
  


  llvm::Constant *haltFunc = myModule->getOrInsertFunction("_Halt",VFT);
  _builtinFuncs["_Halt"] = llvm::cast<llvm::Function>(haltFunc);


  for(auto func : funcs) {
    assert(func->funcDecl);
    llvm::FunctionType *fType = func->funcDecl->func->getFuncSignature(Context,
								       classes,
								       func->argNames);

    //std::cout << "func name = " << func->entry->name << "\n";
    //llvm::errs() << "ftype = " << *fType << "\n";
    func->myFunction = llvm::Function::Create(fType,
					      llvm::Function::ExternalLinkage,
					      func->entry->name.c_str(), 
					      myModule);
    
    }
  //printf("found %zu classes\n", classes.size());
  for(auto cD : classes) {
    cD->makeVTable(Context, myModule, myIRBuilder, 
		   labelMap, vtableNames);
  }

  for(auto func : funcs) {
    func->generateLLVM(myIRBuilder, Context, myModule);
  }
  

  std::string _errors;
  llvm::raw_string_ostream OS(_errors);
  
  if(llvm::verifyModule(*myModule, &OS)) {
    printf("==> bad IR! <==\n");
    exit(-1);
  }
  printf("==> good IR! <==\n");
  
  const std::string bitname= "decaf.bc";
  int fd = open(bitname.c_str(), O_RDWR|O_CREAT, (S_IRUSR | S_IWUSR) );
  llvm::raw_fd_ostream bcOut(fd, false, false);
  llvm::WriteBitcodeToFile(myModule, bcOut);
  //bcOut.close();
  //close(fd);
    
}


