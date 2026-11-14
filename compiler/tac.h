/* File: tac.h
 * -----------
 * This module contains the Instruction class (and its various
 * subclasses) that represent Tac instructions and the Location
 * class used for operands to those instructions.
 *
 * Each instruction is mostly just a little struct with a
 * few fields, but each responds polymorphically to the methods
 * Print and Emit, the first is used to print out the TAC form of
 * the instruction (helpful when debugging) and the second to
 * convert to the appropriate MIPS assembly.
 *
 * The operands to each instruction are of Location class.
 * A Location object is a simple representation of where a variable
 * exists at runtime, i.e. whether it is on the stack or global
 * segment and at what offset relative to the current fp or gp.
 *
 * You may need to make changes/extensions to these classes
 * if you are working on IR optimization.

 */

#ifndef _H_tac
#define _H_tac

#include <set>
#include "llvmInc.hh"

#include "list.h" // for VTable
class Mips;


    // A Location object is used to identify the operands to the
    // various TAC instructions. A Location is either fp or gp
    // relative (depending on whether in stack or global segemnt)
    // and has an offset relative to the base of that segment.
    // For example, a declaration for integer num as the first local
    // variable in a function would be assigned a Location object
    // with name "num", segment fpRelative, and offset -8. 
 
typedef enum {fpRelative, gpRelative} Segment;

class Variable;
class rVariable;

class Location
{
 public:
  const char *variableName;
  Segment segment;
  int offset;
  bool isFloat;
  bool isFuncArg;
  std::string funcArgName;
  Location(Segment seg, int offset, const char *name);
  const char *GetName()           { return variableName; }
  Segment GetSegment()            { return segment; }
  int GetOffset()                 { return offset; }
  bool GetFloat()                 { return isFloat; }
  void SetFloat(bool isFloat)     { this->isFloat = isFloat; } 
  bool isGlobal() {return this->segment == gpRelative;}
};
 


  // base class from which all Tac instructions derived
  // has the interface for the 2 polymorphic messages: Print & Emit





class Instruction {
    protected:
      char printed[128];
      
    public:
      typedef enum {
	LoadConstant=0, 
	LoadStringConstant=1,
	LoadLabel=2, 
	Assign=3,
	Load=4,
	Store=5,
	BinaryOp=6, 
	Label=7,
	Goto=8, 
	IfZ=9,
	BeginFunc=10,
	EndFunc=11,
	Return=12, 
	PushParam=13,
	PopParams=14, 
	LCall=15,
	ACall=16,
	VTable=17,
	NumTAC=18
      }
      TACTypes;
      virtual ~Instruction() {}
      virtual void Print();
      virtual void PrintSSA() {}
      virtual std::string asString();
      virtual void getLocations(std::set<Location*> &locs);
      virtual TACTypes getType() { return Instruction::NumTAC; }
      virtual void EmitSpecific(Mips *mips) = 0;
      virtual void Emit(Mips *mips);
      
      virtual size_t numOperands() const {
	return 0;
      }
      virtual Location *getLocation(size_t id) const {
	return nullptr;
      }
      virtual Location *getDst() {
	return nullptr;
      }
};

  
  
  // for convenience, the instruction classes are listed here.
  // the interfaces for the classes follows below
  
class LoadConstant;
class LoadStringConstant;
class LoadLabel;
class Assign;
class Load;
class Store;
class BinaryOp;
class Label;
class Goto;
class IfZ;
class BeginFunc;
class EndFunc;
class Return;
class PushParam;
class PopParams;
class LCall;
class ACall;
class VTable;
class FnDecl;

class LoadConstant: public Instruction {
 public:
  Location *dst;
  rVariable *rDst;
  int val;
  LoadConstant(Location *dst, int val);
  void EmitSpecific(Mips *mips) override;
  Location *getDst() override {
    return dst;
  }
  void getLocations(std::set<Location*> &locs) override {
    locs.insert(dst);
  }
  TACTypes getType() override { return Instruction::LoadConstant; }
  void PrintSSA() override;
};

class LoadStringConstant: public Instruction {
 public:
  Location *dst;
  rVariable *rDst;
  char *str;
  LoadStringConstant(Location *dst, const char *s);
  void EmitSpecific(Mips *mips) override;
  Location *getDst() override {
    return dst;
  }
  void getLocations(std::set<Location*> &locs) override {
    locs.insert(dst);
  }
  TACTypes getType() override { return Instruction::LoadStringConstant; }
  void PrintSSA() override;
};
    
class LoadLabel: public Instruction {
 public:
  Location *dst;
  rVariable *rDst;
  const char *label;
  LoadLabel(Location *dst, const char *label);
  void EmitSpecific(Mips *mips) override;
  Location *getDst() override {
    return dst;
  }
  void getLocations(std::set<Location*> &locs) override {
    locs.insert(dst);
  }
  TACTypes getType() override {
    return Instruction::LoadLabel;
  }
  void PrintSSA() override;
};

class Assign: public Instruction {
 public:
  Location *dst, *src;
  rVariable *rDst, *rSrc;
  Assign(Location *dst, Location *src);
  void EmitSpecific(Mips *mips) override;
  Location *getDst() override {
    return dst;
  }
  void getLocations(std::set<Location*> &locs) override {
    locs.insert(dst); locs.insert(src);
  }
  TACTypes getType() override {
    return Instruction::Assign;
  }
  void PrintSSA() override;
  
  size_t numOperands() const override {
    return 1;
  }
  Location *getLocation(size_t id) const override{
    switch(id)
      {
      case 0:
	return src;
      default:
	break;
      }
    return nullptr;
  }
};

class Load: public Instruction {
 public:
  Location *dst, *src;
  rVariable *rDst, *rSrc;
  int offset;
  Load(Location *dst, Location *src, int offset = 0);
  void EmitSpecific(Mips *mips) override;
  Location *getDst() override {
    return dst;
  }
  void getLocations(std::set<Location*> &locs) override {
    locs.insert(dst); locs.insert(src);
  }
  TACTypes getType() override { return Instruction::Load; }
  void PrintSSA() override;

  size_t numOperands() const override {
    return 1;
  }
  Location *getLocation(size_t id) const override {
    switch(id)
      {
      case 0:
	return src;
      default:
	break;
      }
    return nullptr;
  }
};

class Store: public Instruction {
 public:
  Location *dst, *src;
  rVariable *rDst, *rSrc;
  int offset;
  Store(Location *d, Location *s, int offset = 0);
  void EmitSpecific(Mips *mips) override;
  Location *getDst() override {
    return dst;
  }
  void getLocations(std::set<Location*> &locs) override {
    locs.insert(dst); locs.insert(src);
  }
  TACTypes getType() override { return Instruction::Store; }
  void PrintSSA() override;

  Location *getLocation(size_t id) const override {
    switch(id)
      {
      case 0:
	return src;
      case 1:
	return dst;
      default:
	break;
      }
    return nullptr;
  }
  size_t numOperands() const override {
    return 2;
  }
  
};

class BinaryOp: public Instruction {
 public:
  typedef enum {Add, Sub, Mul, Div, Mod, Eq, Less, And, Or, NumOps} OpCode;
  static const char * const opName[NumOps];
  static const char * const dotOpName[NumOps];
  static OpCode OpCodeForName(const char *name);
  OpCode code;
  Location *dst, *op1, *op2;
  rVariable *rDst, *rOp1, *rOp2;
  bool isFloat;
  std::string dotString;
  BinaryOp(OpCode c, Location *dst, Location *op1, Location *op2);
  void EmitSpecific(Mips *mips) override;
  std::string asString() override;
  Location *getDst() override {
    return dst;
  }
  void getLocations(std::set<Location*> &locs) override {
    locs.insert(dst);
    locs.insert(op1);
    locs.insert(op2);
  }
  TACTypes getType() override {
    return Instruction::BinaryOp;
  }
  void PrintSSA() override;
  Location *getLocation(size_t id) const override {
    switch(id)
      {
      case 0:
	return op1;
      case 1:
	return op2;
      default:
	break;
      }
    return nullptr;
  }
  size_t numOperands() const override {
    return 2;
  }
};

class Label: public Instruction {
 public:
  const char *label;
  Label(const char *label);
  std::string getLabel() { return std::string(label); }
  TACTypes getType() override { return Instruction::Label; }
  void Print() override;
  void EmitSpecific(Mips *mips) override;
};

class Goto: public Instruction {
 public:
  const char *label;
  Goto(const char *label);
  std::string getLabel() { return std::string(label); }
  TACTypes getType() override { return Instruction::Goto; }
  void EmitSpecific(Mips *mips) override;
  void PrintSSA() override;
};

class IfZ: public Instruction {
 public:
  Location *test;
  const char *label;
  rVariable *rTest;
  IfZ(Location *test, const char *label);
  void EmitSpecific(Mips *mips) override;
  void PrintSSA() override;
  std::string getLabel() { 
    return std::string(label);
  }
  TACTypes getType() override {
    return Instruction::IfZ;
  }
  void getLocations(std::set<Location*> &locs) override {
    locs.insert(test);
  }
  Location *getLocation(size_t id) const override {
    switch(id)
      {
      case 0:
	return test;
      default:
	break;
      }
    return nullptr;
  }
  size_t numOperands() const override {
    return 1;
  }
  
};



class BeginFunc: public Instruction {
 public:
  FnDecl *func;
  int frameSize;
  BeginFunc();
  // used to backpatch the instruction with frame size once known
  TACTypes getType() override { return Instruction::BeginFunc; }
  void SetFrameSize(int numBytesForAllLocalsAndTemps);
  void EmitSpecific(Mips *mips) override;
};

class EndFunc: public Instruction {
 public:
  EndFunc();
  TACTypes getType() override { return Instruction::EndFunc; }
  void EmitSpecific(Mips *mips) override;
};

class Return: public Instruction {
 public:
  Location *val;
  rVariable *rVal;
  Return(Location *val);
  void EmitSpecific(Mips *mips) override;
  void getLocations(std::set<Location*> &locs) override {
    locs.insert(val);
  }
  TACTypes getType() override {
    return Instruction::Return;
  }
  void PrintSSA() override;
  Location *getLocation(size_t id) const override {
    switch(id)
      {
      case 0:
	return val;
      default:
	break;
      }
    return nullptr;
  }
  size_t numOperands() const override {
    return 1;
  }
  
};   

class PushParam: public Instruction {
 public:
  Location *param;
  rVariable *rParam;
  PushParam(Location *param);
  void EmitSpecific(Mips *mips) override;
  void PrintSSA() override;
  void getLocations(std::set<Location*> &locs) override {
    locs.insert(param);
  }
  TACTypes getType() override {
    return Instruction::PushParam;
  }
  Location *getLocation(size_t id) const override {
    switch(id)
      {
      case 0:
	return param;
      default:
	break;
      }
    return nullptr;
  }
  size_t numOperands() const override {
    return 1;
  }
}; 

class PopParams: public Instruction {
 public:
  int numBytes;
  PopParams(int numBytesOfParamsToRemove);
  TACTypes getType() override {
    return Instruction::PopParams;
  }
  void EmitSpecific(Mips *mips) override;
}; 

class LCall: public Instruction {
 public:
  const char *label;
  Location *dst;
  rVariable *rDst;
  LCall(const char *label, Location *result);
  void EmitSpecific(Mips *mips) override;
  std::string getLabel() {
    return std::string(label);
  }
  Location *getDst() override {
    return dst;
  }
  void getLocations(std::set<Location*> &locs) override {
    locs.insert(dst);
  }
  TACTypes getType() override {
    return Instruction::LCall;
  }
  void PrintSSA() override;
};

class ACall: public Instruction {
 public:
  Location *dst, *methodAddr;
  rVariable *rDst, *rMethodAddr;
  ACall(Location *meth, Location *result);
  void EmitSpecific(Mips *mips) override;
  Location *getDst() override {
    return dst;
  }
  void getLocations(std::set<Location*> &locs) override {
    locs.insert(dst);
    locs.insert(methodAddr);
  }
  TACTypes getType() override {
    return Instruction::ACall;
  }
  Location *getLocation(size_t id) const override {
    switch(id)
      {
      case 0:
	return methodAddr;
      default:
	break;
      }
    return nullptr;
  }
  size_t numOperands() const override {
    return 1;
  }
  void PrintSSA() override;
};

class VTable: public Instruction {
 public:
  List<const char *> *methodLabels;
  const char *label;
  VTable(const char *labelForTable, List<const char *> *methodLabels);
  TACTypes getType() override { return Instruction::VTable; }
  void Print() override;
  void EmitSpecific(Mips *mips) override;
};


#endif
