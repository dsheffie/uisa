/* File: ast_stmt.cpp */

#include "ast_stmt.h"
#include "ast_type.h"
#include "ast_decl.h"
#include "ast_expr.h"
#include "debug.h"
#include "errors.h"
//pp4
#include "codegen.h"

extern bool enable_float;

Program::Program(List<Decl*> *d) 
{
  assert(d != NULL);
  (decls=d)->setParentAll(this);
}



/* pp3: here is where the semantic analyzer is kicked off.
 *      The general idea is perform a tree traversal of the
 *      entire program, examining all constructs for compliance
 *      with the semantic rules.  Each node can have its own way of
 *      checking itself, which makes for a great use of inheritance
 *      and polymorphism in the node classes.
 */


bool Program::check() 
{

  Check *c = new Check;
  /*register class and function declarations to the scope*/
  decls->getInterfaceSignatures(c);
  decls->getClassSignatures(c);
  decls->getFnSignatures(c);

  /*check the global variable declarations*/
  decls->checkVarDecls(c);

  /*check the interface declarations*/
  decls->checkInterfaceDecls(c);
  /*check the class declarations*/
  decls->checkClassDecls(c);
  /*check the function delcarations*/
  decls->checkFnDecls(c);

 
  //make sure we have a proper main()
  this->main = c->programHasMain();

  
  return (ReportError::NumErrors() == 0);
}

bool Program::check(Check *c) 
{
  return this->check();
}


//pp4
void Program::emitprog(std::string &srcName) {
  /* create new CodeGenerator object, extern 
   * declaration in codegen.h
   */
  cg = new CodeGenerator;
  //gen vtables
  decls->generateAllVTables();
  
  /* start things off by calling the emit method
   * on all declarations */
  decls->emitAll();
  if(main) {
    /* Output MIPS assembly */
    cg->DoFinalCodeGen(srcName);
  } 
  delete cg;
}


void Program::printChildren(int indentLevel) {
  decls->printAll(indentLevel+1);
  printf("\n");
}

//for pp4
void StmtBlock::emit()
{
  //hmm
  Scope *tempScope = emitScope;
  emitScope = this->myScope;
  decls->emitAll(); //emit decls first
  stmts->emitAll();
  emitScope = tempScope;
}

StmtBlock::StmtBlock(List<VarDecl*> *d, List<Stmt*> *s) {
  assert(d != NULL && s != NULL);
  (decls=d)->setParentAll(this);
  (stmts=s)->setParentAll(this);
}

void StmtBlock::printChildren(int indentLevel) {
  decls->printAll(indentLevel+1);
  stmts->printAll(indentLevel+1);
}

/*check all children of a stmtblock*/
bool StmtBlock::checkChildren(Check *c)
{
  /*stmt block is "ok" if both decl and stmt checks are "ok"*/
  bool decls_check = true;
  bool stmts_check = true;
  
  /* Entering into a new scope,
   * Save the previous scope*/
  //Scope *savedScope;

  //check if our parent is a fndecl
  FnDecl *myParent = 
    dynamic_cast<FnDecl*>(parent);

  bool myParentIsFn = 
    (myParent != static_cast<FnDecl*>(NULL));
  
  Scope *savedScope = c->getScope();
  if(!myParentIsFn)
    {
      c->setPreviousScope(savedScope);
      c->newScope();
      c->resetNameCheck();
      c->setIdentifier();      
      /*copy the previous scope to the new scope*/
      c->mergeScope();
    }
  /*check the decls in of the stmtblock*/
  c->resetNameCheck();
  decls_check = decls->checkVarDecls(c);
  c->clearIdentifier();
  //pp4
  myScope = c->getScope();
  /*check the statements of the stmtblock*/
  stmts_check = stmts->checkAll(c);

  /*stmtblock check is done, pop in the old scope*/
  if(!myParentIsFn)
    {
      c->popScope(savedScope);
    }
  
  return decls_check && stmts_check;
}



void ConditionalStmt::emit()
{


}

ConditionalStmt::ConditionalStmt(Expr *t, Stmt *b) {
  assert(t != NULL && b != NULL);
  (test=t)->setParent(this);
  (body=b)->setParent(this);
}


ForStmt::ForStmt(Expr *i, Expr *t, Expr *s, Stmt *b): LoopStmt(t, b) {
  assert(i != NULL && t != NULL && s != NULL && b != NULL);
  (init=i)->setParent(this);
  (step=s)->setParent(this);
}

/*ensure that conditionalstmt test produces a bool*/
bool ConditionalStmt::validTest(Check *c)
{
  /*first check if the expr is semantic valid*/
  bool valid = true;  
  valid = test->check(c);

  /*if expr is valid, check its type is bool*/
  if(valid)
    {
      assert(test->ExprType);
      if(*(test->ExprType) !=  *(Type::boolType))
	{
	  ReportError::TestNotBoolean(test);
	  valid = false;
	}
      else
	{
	  valid = true;
	}
    }
  else
    {
      valid = false;
    }
  return valid;
}

//for PP4
//lots of code but works, label and labels
void ForStmt::emit()
{
  char* begin_for_label = cg->NewLabel();
  char* end_for_label = cg->NewLabel();
  if(init->myType() != Type::voidType){
    init->emit();
  }
  cg->GenLabel(begin_for_label);
  test->emit();
  cg->GenIfZ(test->GetLocation(),end_for_label);
  char* previous_loop_end_label = loop_end_label;
  loop_end_label = end_for_label;
  body->emit();
  if(step->myType() != Type::voidType){
    step->emit();
  }
  loop_end_label =  previous_loop_end_label;
  cg->GenGoto(begin_for_label);
  cg->GenLabel(end_for_label);

}

void ForStmt::printChildren(int indentLevel) {
  init->print(indentLevel+1, "(init) ");
  test->print(indentLevel+1, "(test) ");
  step->print(indentLevel+1, "(step) ");
  body->print(indentLevel+1, "(body) ");
}

/*check the validity of the for stmts*/
bool ForStmt::checkChildren(Check* c)
{
  /*valid only if all these components are valid*/
  bool init_check = true;
  bool test_check = true;
  bool step_check = true;
  bool body_check = true;

  init_check = init->check(c);
  test_check = this->validTest(c);
  step_check = step->check(c);

  bool prevState = c->setLoop(true);

  body_check = body->check(c);

  c->setLoop(prevState);
  
  return init_check && test_check && step_check && body_check;

}

//for pp4
//lots of code but works, even more labels
void WhileStmt::emit()
{
  char* begin_while_label = cg->NewLabel();
  char* end_while_label = cg->NewLabel();
  cg->GenLabel(begin_while_label);
  test->emit();
  cg->GenIfZ(test->GetLocation(),end_while_label);
  char* previous_loop_end_label = loop_end_label;
  loop_end_label = end_while_label;
  body->emit();
  loop_end_label =  previous_loop_end_label;
  cg->GenGoto(begin_while_label);
  cg->GenLabel(end_while_label);
}

void WhileStmt::printChildren(int indentLevel) {
  test->print(indentLevel+1, "(test) ");
  body->print(indentLevel+1, "(body) ");
}

/*check the validity of the while stmts*/
bool WhileStmt::checkChildren(Check *c)
{
  /*valid only if all these components are valid*/
  bool test_check = true;
  bool body_check = true;

  test_check =  this->validTest(c);

  bool prevState = c->setLoop(true);

  body_check =  body->check(c);

  c->setLoop(prevState);

  return test_check && body_check;
}

//for PP4
//fuck does the label ever stop?
void IfStmt::emit()
{
  test->emit();
  Location *test_location = test->GetLocation();
  char* else_label = cg->NewLabel();
  char* end_if_label = cg->NewLabel();
  cg->GenIfZ(test_location,else_label);
  body->emit();  cg->GenGoto (end_if_label);
  cg->GenLabel(else_label);
  if(elseBody){
    elseBody->emit();
  }
  cg->GenLabel(end_if_label);
}


IfStmt::IfStmt(Expr *t, Stmt *tb, Stmt *eb): ConditionalStmt(t, tb) {
  assert(t != NULL && tb != NULL); // else can be NULL
  elseBody = eb;
  if (elseBody) elseBody->setParent(this);
}


void IfStmt::printChildren(int indentLevel) {
  test->print(indentLevel+1, "(test) ");
  body->print(indentLevel+1, "(then) ");
  if (elseBody) elseBody->print(indentLevel+1, "(else) ");
}

/*check the validity of the if stmts*/
bool IfStmt::checkChildren(Check *c)
{
  /*valid only if all these components are valid*/
  bool test_check = true;
  bool body_check = true;
  bool else_check = true;

  test_check = this->validTest(c);

  body_check = body->check(c);

  if(elseBody) else_check = elseBody->check(c);

  return test_check && body_check && else_check;
}


/* the return stmt will return any expression, or a useless temp variable
 */
void ReturnStmt::emit()
{
 Location *r = 0;
  if(expr->myType()!=Type::voidType){
    expr->emit();
    cg->GenReturn(expr->GetLocation());
  } 
  //returns a usesless variable in case of void return
  else 
    {
      r = cg->GenTempVar();
      if(expr->myType() == Type::doubleType)
	{
	  assert(enable_float);
	  r->SetFloat(true);
	}
      cg->GenReturn(r);
    }
}


ReturnStmt::ReturnStmt(yyltype loc, Expr *e) : Stmt(loc) {
  assert(e != NULL);
  (expr=e)->setParent(this);
}

void ReturnStmt::printChildren(int indentLevel) {
  expr->print(indentLevel+1);
}

/*check the validity of the return stmts*/
bool ReturnStmt::checkChildren(Check *c)
{
  /*return expression could just be a void*/
  bool valid_return = expr->check(c);

  /*now check if the function return type matches
   *the return type*/
  FnDecl* fd = c->getFnScope();
  
  assert(fd);

  Type *fn_return = fd->getReturn();

  assert(fn_return);

  valid_return = Type::CompareTypes(c, fn_return, expr->ExprType);


  if(!valid_return)
    ReportError::ReturnMismatch(this, expr->ExprType, fn_return);
   
  return valid_return;
}

//for pp4
//there is global variable loop_end_label, don't need to be global, but
//just easier for now, it keeps track of which loop to break out of
void BreakStmt::emit()
{
  cg->GenGoto(loop_end_label);
  
}

/*check the validity of the break stmts*/
bool BreakStmt::checkChildren(Check *c)
{
  /*just make sure we are actually in a loop*/
  bool break_okay = c->inLoop();
  
  if(!break_okay)
    {
      ReportError::BreakOutsideLoop(this);
    }
  return break_okay;
}

//for pp4
void PrintStmt::emit()
{
  //generate labels, etc
  args->emitAll();
  
  for(int i = 0; i < args->size(); i++)
    {
      Expr* e = args->get(i);

      if(e->myType() == Type::stringType)
	{
	  cg->GenBuiltInCall(PrintString, e->GetLocation(),
			   NULL);
	}
      
      else if(e->myType() == Type::intType)
	{
	  cg->GenBuiltInCall(PrintInt, e->GetLocation(),
			     NULL);
	}

      else if(e->myType() == Type::boolType)
	{
	  cg->GenBuiltInCall(PrintBool, e->GetLocation(),
			     NULL);

	}

      else if(enable_float && 
	      (e->myType() == Type::doubleType))
	{
	  cg->GenPrintFloat(e->GetLocation());
	}

    }
 }


PrintStmt::PrintStmt(List<Expr*> *a) {
  assert(a != NULL);
  (args=a)->setParentAll(this);
}

void PrintStmt::printChildren(int indentLevel) {
  args->printAll(indentLevel+1, "(args) ");
}


/*check the validity of the print stmts*/
bool PrintStmt::checkChildren(Check *c)
{
  /*check all expression in the arguments*/
  bool args_check = args->checkAll(c);

  for(int i = 0; i < args->size(); i++)
    {
      Expr *e = args->get(i);
      assert(e->ExprType);
      Type *t = e->ExprType;

      /*type check the print stmt arguments*/
      bool valid_print = (
			  (*t == *Type::intType) ||
			  (*t == *Type::stringType) ||
			  (*t == *Type::boolType) ||
			  ((*t == *Type::doubleType) && enable_float));
      
      if(!valid_print && (*t != *Type::errorType)){
	ReportError::PrintArgMismatch(e, i+1, e->ExprType);
      }
    }
  
  return args_check;
}

CaseStmt::CaseStmt(Expr *ci, List<Stmt*> *s){
  assert(ci!=NULL && s!=NULL);
  (caseint = ci)->setParent(this);
  (stmt = s)->setParentAll(this);
}

bool CaseStmt::checkChildren(Check *c){

  bool expr_check  = caseint->check(c);
  bool stmt_check = true;
  for(int i  = 0; i<stmt->size(); i++){
    if(!stmt->get(i)->check(c)){
      stmt_check = false;
    }
  }
  return expr_check && stmt_check;
}

/* the content of the case stmts only require simple emit*/
void CaseStmt::emit(){
  for(int i  = 0; i<stmt->size(); i++){
    stmt->get(i)->emit();
  }
}

void CaseStmt::printChildren(int indentLevel){
  caseint->print(indentLevel+1);
  stmt->printAll(indentLevel+1);
}
SwitchStmt::SwitchStmt(Expr *t, List<CaseStmt*> *b, List<Stmt*> *d){
  assert(t!=NULL && b!=NULL);
  test = t;
  casebody = b;
  defaultbody = d;
  int i = 0; 
  for(i = 0; i<casebody->size(); i++){
    casebody->get(i)->setParent(this);
  }
  if(defaultbody) defaultbody->setParentAll(this);
}

/*in additionl to checking each component, we store the intconst
 *of each case statment in a special tests list to help code gen*/
bool SwitchStmt::checkChildren(Check *c){

  tests = new List<Expr*>;
  bool expr_test = test->check(c);
  bool prevState = c->setLoop(true);
  bool case_test = true;
  for(int i = 0; i< casebody->size(); i++){
    if(!casebody->get(i)->check(c)){
      case_test = false;
    }
    tests->append(casebody->get(i)->getTest());
  }
  bool default_test = true;
  for(int i = 0; i<defaultbody->size(); i++){
    if(!defaultbody->get(i)->check(c)){
      default_test = false;
    }
  }

  c->setLoop(prevState);
  return expr_test && case_test && default_test;
}

/*Switch emit put all the tests up in the front, and uses alot of
 *label and jumps to navigate between the cases, if no break
 *is present, execution continues on the next case*/
void SwitchStmt::emit(){
  /*Switch is a "loop" statment which break is allowed
   *end of switch label is necessary to navigate these jumps*/
  char* end_switch_label = cg->NewLabel();
  test->emit();
  /*each case stmt has a label, all of them are stored in this array*/
  char** case_labels = (char**)malloc(casebody->size()*sizeof(char*));
  char* default_label = cg->NewLabel();
  /*create new labels and emit all the case int consts*/
  for(int i = 0; i<casebody->size(); i++){
    case_labels[i] = cg->NewLabel();
    tests->get(i)->emit();
  }
  /*setup end of loop label for break stmts*/
  char* previous_loop_end_label = loop_end_label;
  loop_end_label = end_switch_label;
  Location* zero = cg->GenLoadConstant(0);

  for(int i = 0; i<tests->size(); i++){
    /*for each case stmt, check if expr == case's intconst*/
    Location* equal = cg->GenBinaryOp("==",
				      test->GetLocation(),
				      tests->get(i)->GetLocation());

    Location* notequal = cg->GenBinaryOp("==",
					 equal,
					 zero);
    /*if matched then jump to label*/
    cg->GenIfZ(notequal,case_labels[i]);
    if(i < tests->size()-1){
      /*if didn't match this case, move to the next one*/
    } else {
      /*if nothing matched, then jump to default*/
      cg->GenIfZ(equal,default_label);
    }
  }

  /*simply geneate case stmts with their labels*/
  for(int i = 0; i<casebody->size(); i++){
    cg->GenLabel(case_labels[i]);
    casebody->get(i)->emit();
  }	     

  /*generate default*/
  cg->GenLabel(default_label);
  for(int i = 0 ; i< defaultbody->size(); i++){
    defaultbody->get(i)->emit();
  }
  loop_end_label =  previous_loop_end_label;
  cg->GenLabel(end_switch_label);
}

void SwitchStmt::printChildren(int indentLevel){
  test->print(indentLevel+1, "(test) ");
  int i = 0;
  for(i = 0; i<casebody->size(); i++){
    casebody->get(i)->print(indentLevel+1, "(case) ");
  }
  if(defaultbody) defaultbody->printAll(indentLevel+1, "(default) ");
}

