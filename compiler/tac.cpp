/* File: tac.cpp
 *
 * Implementation of Location class and Instruction class/subclasses.
 */

#include "tac.h"
#include "mips.h"
#include "debug.h"
#include "codegen.h"
#include <cstdio>

extern bool enable_float;

Location::Location(Segment s, int o, const char *name) :
  variableName(strdup(name)), segment(s), offset(o), isFloat(false), isFuncArg(false)
{ }


void Instruction::Print() {
  printf("\t%s ;\n", printed);
}

std::string Instruction::asString() {
  return std::string(printed);
}


void Instruction::getLocations(std::set<Location*> &locs)
{
  return;
}


void Instruction::Emit(Mips *mips) {
  if (*printed)
    mips->Emit("# %s", printed);   // emit TAC as comment into assembly
  EmitSpecific(mips);
}

LoadConstant::LoadConstant(Location *d, int v)
  : dst(d), val(v) {
  assert(dst != NULL);
  sprintf(printed, "%s = %d", dst->GetName(), val);
}
void LoadConstant::EmitSpecific(Mips *mips) {
  mips->EmitLoadConstant(dst, val);
}

void LoadConstant::PrintSSA()
{
  assert(rDst);
  printf("%s = %d\n", rDst->getName().c_str(), val);
}

LoadStringConstant::LoadStringConstant(Location *d, const char *s)
  : dst(d), rDst(0) {
  assert(dst != NULL && s != NULL);
  const char *quote = (*s == '"') ? "" : "\"";
  str = new char[strlen(s) + 2*strlen(quote) + 1];
  sprintf(str, "%s%s%s", quote, s, quote);
  quote = (strlen(str) > 50) ? "...\"" : "";
  sprintf(printed, "%s = %.50s%s", dst->GetName(), str, quote);
}
void LoadStringConstant::EmitSpecific(Mips *mips) {
  mips->EmitLoadStringConstant(dst, str);
}

void LoadStringConstant::PrintSSA()
{
  assert(rDst);
  printf("%s = %s\n", rDst->getName().c_str(), 
	 str);
}

LoadLabel::LoadLabel(Location *d, const char *l)
  : dst(d), rDst(0), label(strdup(l)) {
  assert(dst != NULL && label != NULL);
  sprintf(printed, "%s = %s", dst->GetName(), label);
}
void LoadLabel::EmitSpecific(Mips *mips) {
  mips->EmitLoadLabel(dst, label);
}

void LoadLabel::PrintSSA()
{
  assert(rDst);
  printf("%s = %s\n", rDst->getName().c_str(),
	 label);
}


Assign::Assign(Location *d, Location *s)
  : dst(d), src(s), rDst(0), rSrc(0) 
{
  assert(dst != NULL && src != NULL);
  sprintf(printed, "%s = %s", dst->GetName(), src->GetName());
}
void Assign::EmitSpecific(Mips *mips) {
  mips->EmitCopy(dst, src);
}

void Assign::PrintSSA()
{
  assert(rDst);
  assert(rSrc);
  printf("%s = %s\n", 
	 rDst->getName().c_str(), 
	 rSrc->getName().c_str());
}

Load::Load(Location *d, Location *s, int off)
  : dst(d), src(s), rDst(0), rSrc(0), offset(off) 
{
  assert(dst != NULL && src != NULL);
  if (offset)
    sprintf(printed, "%s = *(%s + %d)", dst->GetName(), src->GetName(), offset);
  else
    sprintf(printed, "%s = *(%s)", dst->GetName(), src->GetName());
}

void Load::EmitSpecific(Mips *mips) {
  mips->EmitLoad(dst, src, offset);
}

void Load::PrintSSA()
{
  assert(rDst);
  assert(rSrc);
  printf("%s = %s\n", 
	 rDst->getName().c_str(), 
	 rSrc->getName().c_str());
}

Store::Store(Location *d, Location *s, int off)
  : dst(d), src(s), rDst(0), rSrc(0), offset(off) 
{
  assert(dst != NULL && src != NULL);
  if (offset)
    sprintf(printed, "*(%s + %d) = %s", dst->GetName(), offset, src->GetName());
  else
    sprintf(printed, "*(%s) = %s", dst->GetName(), src->GetName());
}
void Store::EmitSpecific(Mips *mips) {
  mips->EmitStore(dst, src, offset);
}

void Store::PrintSSA()
{
  assert(rDst);
  assert(rSrc);
  printf("%s = %s\n", 
	 rDst->getName().c_str(), 
	 rSrc->getName().c_str());
}

const char * const BinaryOp::opName[BinaryOp::NumOps] =
  {"+",	"-", "*", "/", "%", "==", "<", "&&", "||"};
const char * const BinaryOp::dotOpName[BinaryOp::NumOps] =
  {"+", "-", "*", "/", "&", "==", "less", "and", "or"};

BinaryOp::OpCode BinaryOp::OpCodeForName(const char *name) {
  for (int i = 0; i < NumOps; i++)
    if (opName[i] && !strcmp(opName[i], name))
	return (OpCode)i;
  die("Unrecognized Tac operator: '%s'\n", name);
  return Add; // can't get here, but compiler doesn't know that
}

BinaryOp::BinaryOp(OpCode c, Location *d, Location *o1, Location *o2)
  : code(c), dst(d), op1(o1), op2(o2),
    rDst(0), rOp1(0), rOp2(0)
{
  assert(dst != NULL && op1 != NULL && op2 != NULL);
  assert(code >= 0 && code < NumOps);
  sprintf(printed, "%s = %s %s %s", dst->GetName(), op1->GetName(), opName[code], op2->GetName()); 
  dotString = std::string(dst->GetName()) + std::string(" = ") + 
    std::string(op1->GetName()) + std::string(" ") + 
    std::string(dotOpName[code]) + std::string(" ") + 
    std::string(op2->GetName());
}

void BinaryOp::PrintSSA()
{
  assert(rOp1);
  assert(rOp2);
  assert(rDst);

  printf("%s = %s %s %s\n", 
	 rDst->getName().c_str(),
	 rOp1->getName().c_str(),
	 opName[code],
	 rOp2->getName().c_str());
}

std::string BinaryOp::asString()
{
  return dotString;
}


void BinaryOp::EmitSpecific(Mips *mips) 
{  
  
  mips->EmitBinaryOp(code, dst, op1, op2);
}


Label::Label(const char *l) : label(strdup(l)) {
  assert(label != NULL);
  *printed = '\0';
}
void Label::Print() {
  printf("%s:\n", label);
}
void Label::EmitSpecific(Mips *mips) {
  mips->EmitLabel(label);
}


Goto::Goto(const char *l) : label(strdup(l)) {
  assert(label != NULL);
  sprintf(printed, "Goto %s", label);
}
void Goto::EmitSpecific(Mips *mips) {	
  mips->EmitGoto(label);
}

void Goto::PrintSSA()
{

}


IfZ::IfZ(Location *te, const char *l)
  : test(te), label(strdup(l)), rTest(0)
{
  assert(test != NULL && label != NULL);
  sprintf(printed, "IfZ %s Goto %s", test->GetName(), label);
}
void IfZ::EmitSpecific(Mips *mips) {	
  mips->EmitIfZ(test, label);
}

void IfZ::PrintSSA()
{
  assert(rTest);
  printf("IfZ %s Goto %s\n", rTest->getName().c_str(), label);
}

BeginFunc::BeginFunc() : func(0) {
  sprintf(printed,"BeginFunc (unassigned)");
  frameSize = -555; // used as sentinel to recognized unassigned value
}
void BeginFunc::SetFrameSize(int numBytesForAllLocalsAndTemps) {
  frameSize = numBytesForAllLocalsAndTemps;
  sprintf(printed,"BeginFunc %d", frameSize);
}
void BeginFunc::EmitSpecific(Mips *mips) {
  mips->EmitBeginFunction(frameSize);
}

EndFunc::EndFunc() : Instruction() {
  sprintf(printed, "EndFunc");
}
void EndFunc::EmitSpecific(Mips *mips) {
  mips->EmitEndFunction();
}


Return::Return(Location *v) : val(v), rVal(0) 
{
  sprintf(printed, "Return %s", val? val->GetName() : "");
}

void Return::EmitSpecific(Mips *mips) {	
  mips->EmitReturn(val);
}

void Return::PrintSSA()
{
  if(val) {
    assert(rVal);
  }
  printf("Return %s\n", 
	 val ? rVal->getName().c_str() : "");
}

PushParam::PushParam(Location *p)
  :  param(p), rParam(0) 
{
  assert(param != NULL);
  sprintf(printed, "PushParam %s", param->GetName());
}

void PushParam::PrintSSA()
{
  assert(rParam);
  printf("PushParam %s\n", rParam->getName().c_str());
}

void PushParam::EmitSpecific(Mips *mips) {
  mips->EmitParam(param);
}

PopParams::PopParams(int nb)
  :  numBytes(nb) {
  sprintf(printed, "PopParams %d", numBytes);
}
void PopParams::EmitSpecific(Mips *mips) {
  mips->EmitPopParams(numBytes);
}

LCall::LCall(const char *l, Location *d)
  :  label(strdup(l)), dst(d), rDst(0) 
{
  sprintf(printed, "%s%sLCall %s", dst? dst->GetName(): "", dst?" = ":"", label);
}

void LCall::PrintSSA()
{
  if(dst) {
    assert(rDst);
  }
  
  printf("%s%sLCall %s\n", rDst? rDst->getName().c_str(): 
	 "", dst ? " = ":"", label);
}

void LCall::EmitSpecific(Mips *mips) {
  mips->EmitLCall(dst, label);
}

ACall::ACall(Location *ma, Location *d)
  : dst(d), methodAddr(ma), rDst(0), rMethodAddr(0)
{
  assert(methodAddr != NULL);
  sprintf(printed, "%s%sACall %s", dst? dst->GetName(): "", dst?" = ":"",
	    methodAddr->GetName());
}
void ACall::EmitSpecific(Mips *mips) {
  mips->EmitACall(dst, methodAddr);
}

void ACall::PrintSSA()
{

}

VTable::VTable(const char *l, List<const char *> *m)
  : methodLabels(m), label(strdup(l)) {
  assert(methodLabels != NULL && label != NULL);
  sprintf(printed, "VTable for class %s", l);
}

void VTable::Print() {
  printf("VTable %s =\n", label);
  for (int i = 0; i < methodLabels->size(); i++)
    printf("\t%s,\n", methodLabels->get(i));
  printf("; \n");
}
void VTable::EmitSpecific(Mips *mips) {
  mips->EmitVTable(label, methodLabels);
}

