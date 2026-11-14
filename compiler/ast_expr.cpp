/* File: ast_expr.cpp */

#include "ast_expr.h"
#include "ast_type.h"
#include "ast_decl.h"
#include "debug.h"
#include "errors.h"
#include "options.h"
//pp4
#include "codegen.h"


//additional features
extern bool enable_byte;
extern bool enable_float;

EmptyExpr ::EmptyExpr (){
  ExprType = Type::voidType;
}

IntConstant::IntConstant(yyltype loc, int val) : Expr(loc) {
  value = val;
}

void IntConstant::printChildren(int indentLevel) {
  printf("%d", value);
}

bool IntConstant::checkChildren(Check *c) 
{
  ExprType = Type::intType;
  return true;
}

/* PP4: Generate an integer constant */
void IntConstant::emit()
{
  Location *intConst = cg->GenLoadConstant(value);
  //Save location for Integer Constant 
  myLocation = intConst;

}

DoubleConstant::DoubleConstant(yyltype loc, double val) : Expr(loc) 
{
  value = val;
}

void DoubleConstant::emit()
{
  int *v = 0;
  float f = (float)value;
 
  /* Wow, this actually works */
  v = reinterpret_cast<int*>(&f);

  assert(enable_float);


  myLocation = cg->GenLoadConstant(*v);
}



void DoubleConstant::printChildren(int indentLevel) {
  printf("%g", value);
}

bool DoubleConstant::checkChildren(Check *c) 
{
  ExprType = Type::doubleType;
  return true;
}

BoolConstant::BoolConstant(yyltype loc, bool val) : Expr(loc) {
  value = val;
}
void BoolConstant::printChildren(int indentLevel) {
  printf("%s", value ? "true" : "false");
}

bool BoolConstant::checkChildren(Check *c) 
{
  ExprType = Type::boolType;
  return true;
}

/* PP4: Generate a boolean constant */
void BoolConstant::emit()
{
  Location *intConst = cg->GenLoadConstant(value);

  //Save location for BoolConstant 
  myLocation = intConst;

}

StringConstant::StringConstant(yyltype loc, const char *val) : Expr(loc) {
  assert(val != NULL);
  value = strdup(val);
}
void StringConstant::printChildren(int indentLevel) {
  printf("%s",value);
}

/* PP4: Generate a string constant */
void StringConstant::emit()
{
  Location *strConst = cg->GenLoadConstant(value);

  //Save location for StringConstant 
  myLocation = strConst;
}

bool StringConstant::checkChildren(Check *c) 
{
  ExprType = Type::stringType;
  return true;
}

/* PP4: Generate a null constant */
void NullConstant::emit()
{
  this->myLocation = cg->GenLoadConstant(0);
}

bool NullConstant::checkChildren(Check *c)
{
  ExprType = Type::nullType;
  return true;
}

Operator::Operator(yyltype loc, const char *tok) : Node(loc) {
  assert(tok != NULL);
  strncpy(tokenString, tok, sizeof(tokenString));
}

void Operator::printChildren(int indentLevel) {
  printf("%s",tokenString);
}

bool Operator::checkChildren(Check *c) 
{
  return true;
}


CompoundExpr::CompoundExpr(Expr *l, Operator *o, Expr *r)
  : Expr(loc_join(l->getLocation(), r->getLocation())) {
  assert(l != NULL && o != NULL && r != NULL);
  (op=o)->setParent(this);
  (left=l)->setParent(this);
  (right=r)->setParent(this);
}

CompoundExpr::CompoundExpr(Operator *o, Expr *r)
  : Expr(loc_join(o->getLocation(), r->getLocation())) {
  assert(o != NULL && r != NULL);
  left = NULL;
  (op=o)->setParent(this);
  (right=r)->setParent(this);
}
CompoundExpr::CompoundExpr(Expr *l, Operator *o)
  : Expr(loc_join(l->getLocation(), o->getLocation())) {
    assert(l != NULL && o != NULL);
    (op=o)->setParent(this);
    (left=l)->setParent(this);
    (right=NULL);
}

void CompoundExpr::printChildren(int indentLevel) {
  if (left) left->print(indentLevel+1);
  op->print(indentLevel+1);
  if(right) right->print(indentLevel+1);
}

/*check compound expressions*/
bool CompoundExpr::checkChildren(Check *c) 
{
  /*expression valid only if all its components are valid*/
  bool left_check = true;
  bool op_check = true;
  bool right_check = true;

  if (left) left_check = left->check(c);
  op_check = op->check(c);
  if(right) right_check = right->check(c);

  return left_check && op_check && right_check;
}


/* PP4: Generate TAC for an arithmetic expression */
void ArithmeticExpr::emit()
{
  Location *fake_left = 0;
  //If we have 
  if(left)
    {
      left->emit();
      fake_left = left->GetLocation();
    } 
  //Binary operation
  else 
    {
      fake_left = cg->GenLoadConstant(0);
    }
  
  //emit code for rhs of expression
  if(right) {
    right->emit();
  }
  else {
    //unary plus and minus
    Location* temp = nullptr;
    Location* one = cg->GenLoadConstant(1);
    //since this is post increment, just assign;   
    this->SetLocation(cg->GenTempVar());
    cg->GenAssign(this->GetLocation(), 
		  fake_left);
    
    //now increment or decrement
    if(strcmp(op->getOpString(),"++")==0){
      temp = cg->GenBinaryOp("+", fake_left, one);
    }
    else if(strcmp(op->getOpString(),"--")==0){
      temp = cg->GenBinaryOp("-",fake_left, one);
    } else {
      assert(false);
      //invalid, buut shoudn't ever reach here
    }
    //named does store, other wise do assign
    if(left->myType()->isNamed()){
      cg->GenStore(fake_left,temp,0);
    } else {
      cg->GenAssign(fake_left, 
		    temp);
    }
    //all tac generated for unary, just return
    return;
  }
  
  //generate tac
  this->myLocation = 
    cg->GenBinaryOp(op->getOpString(),
		    fake_left, 
		    right->GetLocation());
  
}

/*check arithmetic expressions*/
bool ArithmeticExpr::checkChildren(Check *c)
{
  /*expression valid only if all its components are valid*/
  bool left_check = true;
  bool op_check = true;
  bool right_check = true;

  Type* rhs_type = (Type*)NULL;
  Type* lhs_type = (Type*)NULL;
  
  if (left) left_check = left->check(c);
  op_check = op->check(c);
  if(right) right_check = right->check(c);

  /*all the sub expressions check out*/
  if(left_check && right_check && op_check && right)
    {
      rhs_type = right->ExprType;
      ExprType = rhs_type;

      /*ensure left and right expressions are compatible*/
      if(left)
	{
	  lhs_type =  left->ExprType;
	  if(*rhs_type != *lhs_type)
	    {
	      ReportError::IncompatibleOperands(op, lhs_type, rhs_type);
	      ExprType = Type::errorType;
	      right_check = false;
	    }
	  else
	    {
	      ExprType = rhs_type;
	    }
	}
    }
  //for unary plus and minus, type = left type
  else if(left_check && right_check && op_check && !right){
    lhs_type =  left->ExprType;
    ExprType = lhs_type;
  } 
  if(left_check && op_check && right_check)
    {
      return true;
    }
  else
    {
      ExprType = Type::errorType;
      return false;
    }
}

/* PP4: Emit TAC for an assignment */
void AssignExpr::emit(){
  ArrayAccess *array_check = dynamic_cast<ArrayAccess*>(left);
  FieldAccess *field_check = dynamic_cast<FieldAccess*>(left);

  //if leftside is an array, use store instead of assign
  if(array_check!=NULL)
    {
      array_check->emit_addr();
      right->emit();  
      cg->GenStore(array_check->GetLocation(), right->GetLocation(),0);
    }
  //non-array access
  else 
    {
      VarDecl *temp_var =
	dynamic_cast<VarDecl*>(emitScope->find(field_check->getName())); 

      /*detect implicit this by checking if parent is a classdecl
       *shadowed variables will not have a classdecl as a parent
       *boo ya: syntax directed translation in da house!
       */
      if(!field_check->hasBase() &&
	 dynamic_cast<ClassDecl*>(temp_var->getParent())) 
	{	    
	  field_check->emit_addr();
	  right->emit();  
	  cg->GenStore(field_check->GetLocation(), right->GetLocation(),0);
	}  
      //Not an implicit this
      else 
	{

	  //printf("SCOPE\n");
	  //emitScope->printScope();
	  //printf("__________________________\n");
	  

	  //Explicit this
	  if(field_check->hasThis())
	    {	
	      field_check->emit_addr();
	      right->emit(); 
	      cg->GenStore(field_check->GetLocation(), 
			   right->GetLocation(),0);
	    }
	  //Explicit base (other than this)
	  else if(field_check->hasBase())
	    {
	      //If base is a class (classes are pointers)
	      if(field_check->isPointer())
		{
		  //Emit pointer to lhs
		  field_check->emit_addr();
		}
	      else
		{
		  //dereference lhs and return value
		  field_check->emit_addr();
		}
	      right->emit(); 
	      cg->GenStore(left->GetLocation(), 
			   right->GetLocation(),0);

	    }
	  //Not base, not array, not implicit this
	  else
	    {
	      /* For something like a = b + c
	       * where a, b, and c are integers or
	       * booleans
	       */
	      field_check->emit();
	      right->emit(); 
	      cg->GenAssign(left->GetLocation(), 
			    right->GetLocation());
	    }
	}
    }
}

/*check assignment expressions*/
bool AssignExpr::checkChildren(Check *c)
{
  /*assignment is valid only when its expression is valid*/
  bool left_check = true;
  bool op_check = true;
  bool right_check = true;

  Type* lhs_type = (Type*)NULL;
  /*first assign the type of this expression which is equal to the type
    of left*/


  if (left) left_check = left->check(c);
  op_check = op->check(c);  
  right_check = right->check(c);

  if(left->ExprType!=NULL){
    this->ExprType = left->ExprType;
  }

  /*if the individual expressions check out, then check for  type match*/
  if(right_check && left_check && op_check)
    {         
      assert(right->ExprType);
      /*special case for malloc which returns a void* type */
      if(right->ExprType == Type::specialType){
	return true;
      }
      /*if both expressions are named*/
      if(left->ExprType->isNamed() && (right->ExprType->isNamed() ||
				       right->ExprType->isNull()))
	{
	  NamedType* rhs_type = 
	    dynamic_cast<NamedType*>(right->ExprType);
	  NamedType* lhs_type = 
	    dynamic_cast<NamedType*>(left->ExprType);
	  
	  /*assign null to a named type is allowed*/
	  if(lhs_type && right->ExprType->isNull())
	    {
	      right_check = true;
	    }
	  /*if left right type mismatch, we check the right side for
	   *extended or implemented classes*/
	  else if((*rhs_type != *lhs_type) && (!lhs_type->isNull()))
	    {
	      Decl *d = c->getDecl(right->ExprType->getType());
	      ClassDecl* cd = dynamic_cast<ClassDecl*>(d);
	      /*right side is not a class*/
	      if(!cd)
		{
		  ReportError::IncompatibleOperands(op, 
						    lhs_type, 
						    rhs_type);
		  right_check = false;
		}
	      
	      /*check for extends and implements*/
	      while(cd)
		{
		  /*check the implements first*/
		  List<NamedType *> *implements = cd->getImplementClass();
		  bool check_implement = false; 
		  /*there could be multiple implments, as long as one of
		    them matches*/
		  for(int i = 0; i <implements->size();i++){
		    NamedType *it = implements->get(i);
		    if(*it == *lhs_type)
		      {
			check_implement = true;
			break;
		      }
		  }
		  /*if one of the implements matched, 
		    then assignemnt match*/
		  if(check_implement){
		    right_check = true;
		    break;
		  }

		  /*check for extends second, 
		    and this could lead to more*/

		  NamedType *nt = cd->getBaseClass();
		  /*no more base class exists*/
		  if(!nt) 
		    {
		      ReportError::IncompatibleOperands(op, 
							lhs_type, 
							rhs_type);
		      right_check = false;
		      break;
		    }
		  /*class match*/
		  if(*nt == *lhs_type)
		    {
		      right_check = true;
		      break;
		    }
		  /*if no match, descend into the base class*/
		  d = c->getDecl(nt->getType());
		  cd = dynamic_cast<ClassDecl*>(d);
		}
	    }
	}
      /*if both sides are array types*/
      else if(right->ExprType->isArray() && left->ExprType->isArray())
	{
	  ArrayType* rhs_type =
	    dynamic_cast<ArrayType*>(right->ExprType);
	  ArrayType* lhs_type =
	    dynamic_cast<ArrayType*>(left->ExprType);

	  assert(rhs_type != static_cast<ArrayType*>(NULL));
	  assert(lhs_type != static_cast<ArrayType*>(NULL));
	  
	  if(*rhs_type != *lhs_type)
	    {
	      ReportError::IncompatibleOperands(op, lhs_type, rhs_type);
	      right_check = false;
	    }
	}
      /*othewise do a most basic type compare*/
      else
	{
	  assert(right->ExprType);
	  assert(left->ExprType);

	  Type* rhs_type = right->ExprType;
	  Type* lhs_type = left->ExprType;	  
	  if(*rhs_type != *lhs_type)
	    {
	      ReportError::IncompatibleOperands(op, lhs_type, rhs_type);
	      right_check = false;
	    }
	}
    }
  /*if one of the expression check failed
   *emit the correct error message if the error hasn't already
   *been reported previously*/
  else
    {
      Type *rhs_type = right->ExprType;
      if(left)
	{
	  Type *lhs_type = left->ExprType;
	  if(*lhs_type != *Type::errorType && 
	     *rhs_type != *Type::errorType)
	    {
	      ReportError::IncompatibleOperands(op, lhs_type, rhs_type);
	    }
	}
      else
	{
	  if(*rhs_type != *Type::errorType)
	    {
	      ReportError::IncompatibleOperand(op, rhs_type);
	    }
	}
    }
  return left_check && op_check && right_check;
}

/* PP4: Emit TAC for a logical expresion */
void LogicalExpr::emit()
{
  Location* fake_left = 0;
  if(left)
    {
      /* Generate binary logical operations */
      left->emit();
      right->emit();
      this->myLocation = 
	cg->GenBinaryOp(op->getOpString(),
			left->GetLocation(),
			right->GetLocation());
    } 
  else 
    {
      /* Synthesize logical not */
      fake_left = cg->GenLoadConstant(0);
      right->emit();
      this->myLocation = 
	cg->GenBinaryOp("==",
			fake_left,
			right->GetLocation());
    }
}

/*check logics expressions*/
bool LogicalExpr::checkChildren(Check* c)
{
  /*expression valid only if all its components are valid*/
  bool left_check = true;
  bool op_check = true;
  bool right_check = true;

  Type* rhs_type;
  Type* lhs_type;
  
  if (left) left_check = left->check(c);
  op_check = op->check(c);
  right_check = right->check(c);
  assert(right);
 
  /*if the individual expressions checks out do type check*/
  if(left_check && right_check && op_check)
    {
      rhs_type = right->ExprType;
      if(left)
	{
	  lhs_type =  left->ExprType;
	  if((*rhs_type != *lhs_type) || (*rhs_type != *(Type::boolType)))
	    {
	      ReportError::IncompatibleOperands(op, lhs_type, rhs_type);
	      right_check = false;	  
	    }
	}
      else
	{
	  if(*rhs_type != *(Type::boolType))
	    {
	      ReportError::IncompatibleOperand(op, rhs_type); 
	      left_check = false;
	    }
	}
    }

  /*always return bool type */
  ExprType = Type::boolType;
  return left_check && op_check && right_check;

}

/* PP4: Generate TAC for Relational Expression */
void RelationalExpr::emit()
{
  Location *less = 0;
  Location *equal = 0;
  Location *zero = 0;
  Location *notless = 0;
  Location *notequal = 0;

  //Emit TAC for rhs and lhs
  left->emit();
  right->emit();

  /* Because only "<" and "==" are available,
   * we must synthesize all other relational operators
   */

  //Synthesize ">" operator
  if(strcmp(op->getOpString(),">")==0)
    {
      less = cg->GenBinaryOp("<", 
			     left->GetLocation(),
			     right->GetLocation());

      equal = cg->GenBinaryOp("==",
			      left->GetLocation(),
			      right->GetLocation());

      zero = cg->GenLoadConstant(0);

      notless = cg->GenBinaryOp("==",
				less,
				zero);

      notequal = cg->GenBinaryOp("==",equal,zero);

      myLocation = cg->GenBinaryOp("&&",notless,notequal);
    }
  //Synthesize "<=" operator
  else if(strcmp(op->getOpString(),"<=")==0)
    {
      if(enable_float && left->GetLocation()->GetFloat() 
	 && right->GetLocation()->GetFloat())
	{
	  less
	    = cg->GenFloatLe(left->GetLocation(),right->GetLocation());
	  equal       
	    = cg->GenFloatCmp(left->GetLocation(),right->GetLocation());
	}
      else
	{
	  less
	    = cg->GenBinaryOp("<",left->GetLocation(),right->GetLocation());
	  equal       
	    = cg->GenBinaryOp("==",left->GetLocation(),right->GetLocation());
	}

      myLocation = 
	cg->GenBinaryOp("||",less,equal);
    } 
  //Synthesize ">=" operator
  else if(strcmp(op->getOpString(),">=")==0)
    {
      less = cg->GenBinaryOp("<",
			     left->GetLocation(),
			     right->GetLocation());

      zero = cg->GenLoadConstant(0);

      //Synthesize >= by 
      myLocation = cg->GenBinaryOp("==",less,zero);
    }
  //Default statement, < 
  else 
    {  
      if(enable_float && left->GetLocation()->GetFloat() 
	 && right->GetLocation()->GetFloat())
	{
	  myLocation
	    = cg->GenFloatLe(left->GetLocation(),right->GetLocation());

	}
      else
	{
	  myLocation = cg->GenBinaryOp(op->getOpString(),
				       left->GetLocation(),
				       right->GetLocation());
	}
	
    }
}

/*check relational expressions*/
bool RelationalExpr::checkChildren(Check* c)
{
  /*expression valid only if all its components are valid*/
  bool left_check = true;
  bool op_check = true;
  bool right_check = true;

  Type* rhs_type = (Type*)NULL;
  Type* lhs_type = (Type*)NULL;
  

  if (left) left_check = left->check(c);
  op_check = op->check(c);
  right_check = right->check(c);
  
  /*if all the individual expressions checks ok, then type check*/
  if(left_check && right_check && op_check)
    {
      assert(right);
      assert(left);

      rhs_type =  right->ExprType;
      lhs_type =  left->ExprType;

      /*type check the comparison*/
      bool test = (rhs_type && lhs_type) 
	?
	(*rhs_type == *lhs_type) && 
	((*rhs_type == *Type::doubleType) ||
	 (*rhs_type == *Type::intType))
	: false;

      if(!test)
	{
	  ReportError::IncompatibleOperands(op,lhs_type,rhs_type);
	  right_check = false;
	}      
    }

  /*always return a bool type regardless erros*/
  ExprType = Type::boolType;
  return left_check && op_check && right_check;
}

/* PP4: Generate TAC for an Equality Expression */
void EqualityExpr::emit()
{
  Location *equal = 0;
  Location *zero = 0;
  
  //Generate TAC for lhs and rhs
  left->emit();
  right->emit();
  
  //!= is not a TAC instruction, must be synthesized
  if(strcmp(op->getOpString(),"!=")==0)
    {
      //Synthesize != for strings
      if(left->myType() == Type::stringType)
	{
	  equal =  cg->GenBuiltInCall(StringEqual,
				      left->GetLocation(),
				      right->GetLocation());
	  
	  zero = cg->GenLoadConstant(0);
	  
	  myLocation = cg->GenBinaryOp("==",equal,zero);
	} 

      else if(left->myType() == Type::doubleType)
	{
	  assert(enable_float);
  	  equal = cg->GenFloatCmp(left->GetLocation(),
				  right->GetLocation());
	  zero = cg->GenLoadConstant(0);
	  myLocation = cg->GenBinaryOp("==",equal,zero);
	  
	} 
      //Generate != for all other types
      else 
	{
	  equal = cg->GenBinaryOp("==",
				  left->GetLocation(),
				  right->GetLocation());

	  zero = cg->GenLoadConstant(0);

	  myLocation = cg->GenBinaryOp("==",equal,zero);
	}
      
    } 
  else 
    {
      //Generate equal operation for strings
      if(left->myType() == Type::stringType)
	{
	  myLocation = cg->GenBuiltInCall(StringEqual,
					  left->GetLocation(), 
					  right->GetLocation());
	}
      else if(left->myType() == Type::doubleType)
	{
	  assert(enable_float);
  	  myLocation = cg->GenFloatCmp(left->GetLocation(),
				       right->GetLocation());
	  
	}
      //Generation equal operation for other types
      else 
	{
	  myLocation = cg->GenBinaryOp(op->getOpString(),
				       left->GetLocation(),
				       right->GetLocation());
	}
    }
}

/*check equality expressions*/
bool EqualityExpr::checkChildren(Check *c)
{
  /*expression valid only if all its components are valid*/
  bool left_check = true;
  bool op_check = true;
  bool right_check = true;

 
  if (left) left_check = left->check(c);
  op_check = op->check(c);
  right_check = right->check(c);
  
  Type* rhs_type = (Type*)right->ExprType;
  Type* lhs_type = (Type*)left->ExprType;
  
 
  if(rhs_type && lhs_type)
    {
      if(*rhs_type == *lhs_type)
	right_check = true;
      else
	right_check = false;
 
    }
  else{
    right_check = false;
  }
  /*always return a booltype regardless the errors*/
  ExprType = Type::boolType;
  return left_check && op_check && right_check ;
}

/*check the "This" expressions*/
bool This::checkChildren(Check *c)
{
  /*First ensure we are in a class, other wise "this" is invalid*/
  if(!c->inClass())
    {
      ReportError::ThisOutsideClassScope(this);
      ExprType = Type::errorType;
      return false;
    }
  /*set the expression to the namedtype of the class*/
  myClass = c->inClass();
  
  ExprType = new NamedType(myClass->getId());

  return true;
}

/* PP4: Emit TAC for "this" */
void This::emit()
{
  /* The current "this" context is held in
   * the code generation object */
  myLocation = cg->getThis();
}



ArrayAccess::ArrayAccess(yyltype loc, Expr *b, Expr *s) : LValue(loc) {
  (base=b)->setParent(this);
  (subscript=s)->setParent(this);
}

void ArrayAccess::printChildren(int indentLevel) {
  base->print(indentLevel+1);
  subscript->print(indentLevel+1, "(subscript) ");
}

/* PP4: Driver function to generate a load address
 * from an array access. Used by the two array
 * access code generation methods, emit() and
 * emit_addr()
 */
void ArrayAccess::emitDriver()
{
 char* inbound = 0;
  Location *arraysize = 0;
  Location *notfit = 0;
  Location *notfit1 = 0;
  Location *notfit2 = 0;
  Location *error_message = 0;
  char *not_neg = 0;
  Location *zero = 0;
  Location *notvalid = 0;
  Location *four = 0;
  Location *offset1 = 0;
  Location *offset = 0;
  Location *finaladdress = 0;

  //Emit TAC for the base and subscript
  base->emit();
  subscript->emit();

  if(options.boundsCheck()) {
    inbound = cg->NewLabel();
    //Load array size
    arraysize = cg->GenLoad(base->GetLocation(),0);
    
    /* Check if array subscript outside of the
     * array bounds */
    notfit1 = cg->GenBinaryOp("<", arraysize,
			      subscript->GetLocation());
    
    notfit2 = cg->GenBinaryOp("==",arraysize,
			      subscript->GetLocation());
    
    /* Array access will be out-of-bounds if the
     * subscript is greater or equal to the 
     * array size */
    notfit = cg->GenBinaryOp("||",notfit1,notfit2);
    
    /* Generate conditional test for array length check */
    cg->GenIfZ(notfit,inbound);
    
    error_message = 
      cg->GenLoadConstant(
			  "Decaf runtime error: Array subscript out of bounds");
    
    /* Generate error messagae and terminate */
    cg->GenBuiltInCall(PrintString, error_message,NULL);
    cg->GenBuiltInCall(Halt, NULL, NULL);
    
    
    /* Generate label for inbound array accesses */
    cg->GenLabel(inbound);
    not_neg = cg->NewLabel();
    zero = cg->GenLoadConstant(0);
    
    notvalid = cg->GenBinaryOp("<",subscript->GetLocation(),zero);
    
    /* Check for array accesses below 0 */
    cg->GenIfZ(notvalid,not_neg);
    error_message 
      = cg->GenLoadConstant("Decaf runtime error: Array subscript out of bounds");
    
    /* Generate termination code */
    cg->GenBuiltInCall(PrintString, error_message,NULL);
    cg->GenBuiltInCall(Halt,NULL,NULL); 
    
    /* Subscript access is valid, perform array access */
    cg->GenLabel(not_neg);
  }
  
  /* Multiply subscript by 4 to get byte address from
   * word addreses */
  four = cg->GenLoadConstant(4);
  offset1 = cg->GenBinaryOp("*",four,subscript->GetLocation());

  /* Add an additional 4 bytes to account for array
   * length storage in the first word */
  offset = cg->GenBinaryOp("+",four,offset1);
  finaladdress = cg->GenBinaryOp("+",offset,base->GetLocation());
  
  /* Perform load */
  myLocation = finaladdress;

}


/* PP4: Perform array access, return value */
void ArrayAccess::emit()
{
  /* emitDriver() generates load address and
   * attaches to "this" node */
  emitDriver();
  /* Perform load */
  myLocation = cg->GenLoad(myLocation,0);  
} 

/* PP4: Perform array access, return address */
void ArrayAccess::emit_addr()
{
  /* emitDriver() generates address and
   * attaches it to "this". Nothing else
   * is needed beyond calling emitDriver()
   */
  emitDriver();
} 

/*check array access expressions*/
bool ArrayAccess::checkChildren(Check *c)
{
  /*expression valid only if all its components are valid*/
  bool base_check = true;
  bool subscript_check = true;
  ArrayType* array_type;
  

  base_check = base->check(c);
  subscript_check = subscript->check(c);
  array_type = 
    dynamic_cast<ArrayType*>(base->ExprType);


  /*check if base has been declared as an array*/
  if(!array_type)
    {
      ReportError::BracketsOnNonArray(base);
      ExprType = Type::errorType; 
      base_check = false;
    }
  else
    {
      /*Make sure the array cast worked*/
      assert(array_type !=(ArrayType*)NULL);
      ExprType = array_type->getElemType();
    }

  /*only index arrays with an integer*/
  if(*subscript->ExprType != *Type::intType)
    {
      ExprType = Type::errorType;
      ReportError::SubscriptNotInteger(subscript);
      subscript_check = false;
    }

  return base_check && subscript_check;
}


FieldAccess::FieldAccess(Expr *b, Identifier *f)
  : LValue(b? loc_join(b->getLocation(), f->getLocation()) : *f->getLocation()) {
  assert(f != NULL); // b can be be NULL (just means no explicit base)
  base = b;
  if (base) base->setParent(this);
  (field=f)->setParent(this);
  myClass = 0;
   
}


void FieldAccess::printChildren(int indentLevel) {
  if (base) base->print(indentLevel+1);
  field->print(indentLevel+1);
}


/* PP4: Generate TAC for a field access */
void FieldAccess::emit()
{
  Decl *d = 0;
  VarDecl *vd = 0;
  ClassDecl *cd = 0;
  Location *myAddr = 0;
  Location *myValue = 0;
  int offset = 0;
  Location *field_location = 0;
  char* base_name = 0;
  Location *t0 = 0;

  /* Code generation without an explicit base */
  if(!base)
    {
      /* Generate TAC for the field variable */
      field->emit();

      /* Perform variable lookup in the current
       * scope to determine if the variable 
       * is an implicit "this" variable */
      d = emitScope->find(this->getName());
      vd = dynamic_cast<VarDecl*>(d);
      assert(vd);
      
      /* If the VarDecl has a ClassDecl as it's
       * parent, then we know the variable belongs
       * to class. This means we have to perform
       * a load to access the variable */
      if(dynamic_cast<ClassDecl*>(vd->getParent()))
	{
	  /* Generate load */
	  myAddr = field->GetLocation();
	  myValue = cg->GenLoad(myAddr,0);
	  myLocation = myValue;
	}
      /* If the variable doesn't have base and it's not
       * part of the class, we are directly accessing
       * the variable.  This means we can directly 
       * assign the variable, aka we don't need to do a
       * load because the variable in question is not
       * a pointer */
      else
	{
	  myLocation = field->GetLocation();
	}
    }
  /* FieldAccess has explicit base */
  else
    {
      /* Generate code for this.* */
      if(dynamic_cast<This*>(base))
	{
	  /* The field access must occur within a class */
	  assert(myClass); 
	  
	  /* Emit TAC for the base */
	  base->emit();
	  
	  /* Search for variable offset within the current
	   * class */
	  offset = myClass->findVariableOffset(field->getName());
	  
	  /* Generate load at variable offset within the class */
	  field_location = cg->GenLoad(base->GetLocation(), offset);
	 
	  myLocation = field_location;
	}
      /* For all other field accesses */
      else
	{
	  base_name = base->getName();

	  base->emit();
	  
	  /* Because the base is not "this, we need to find the 
	   * class the base references in order to determine the
	   * method offset */
	  vd = dynamic_cast<VarDecl*>(emitScope->find(base_name));

	  /* Sanity check */
	  assert(vd);
	  
	  /* Get the class declartion referenced by the
	   * current variable */
	  cd = vd->getClass();
	  
	  /* Sanity check */
	  assert(cd);
	  
	  /* Find the offset with in the base class */
	  offset = cd->findVariableOffset(field->getName());
	  
	  /* Perform load */
	  //t0 =  cg->GenLoad(base->GetLocation(), 0);
	  myLocation = cg->GenLoad(base->GetLocation(), offset);
	}

    }
}


/* PP4: Generate address for field access */
void FieldAccess::emit_addr()
{
  int offset = 0;
  Location *field_location = 0;
  Location *t0 = 0;
  Location *t1 = 0;
  ClassDecl* cd = 0;
  VarDecl* vd = 0;
  char *base_name = 0;

  
  //are we involved with a this statement?
  if(!base)
    {
      field->emit();
      this->myLocation = field->GetLocation();
    }
  else
    {
      base->emit();
      base_name = base->getName();
      
      /* If the base is "This" */
      if(dynamic_cast<This*>(base))
	{
	  cd = myClass;
	}      
      /* Otherwise, lookup the class */
      else
	{
	  assert(base_name);
	  vd = dynamic_cast<VarDecl*>(emitScope->find(base_name));
	  assert(vd);
	  cd = vd->getClass();
	  assert(cd);
	}

      offset = cd->findVariableOffset(field->getName());
    
      /* Generate load */
      t0 = base->GetLocation();
      t1 = cg->GenLoadConstant(offset);

      myLocation = cg->GenBinaryOp("+", t0, t1);
    }
}

/* PP4: Determine if FieldAccess base is "this" */
bool FieldAccess::hasThis()
{
  This *This_base = 0;
  if(!base) return false;
  This_base = dynamic_cast<This*>(base);
  if(This_base) return true;
  else return false;
}

/* PP4: Determine if FieldAccess has a base */
bool FieldAccess::hasBase()
{
  if(!base) return false;
  else return true;
}

/* PP4: Determine if FieldAccess is a pointer
 * type. This is basically checking if the 
 * the FieldAccess is a class, but isClass()
 * was already taken by a method used in 
 * PP3 */
bool FieldAccess::isPointer()
{
  VarDecl* vd = 0;
  Decl* d = 0;
  Type* t = 0;

  /* No base, can't be a class */
  if(!base) return false;
  
  /* Find reference from symbol table */
  d = emitScope->find(field->getName());

  /* Get variable declaration */
  vd = dynamic_cast<VarDecl*>(d);

  /* No variable declaration, not a
   * class */
  if(!vd) return false;

  /* Get the variable type */
  t = vd->getVarType();

  /* Named types are classes */
  if(dynamic_cast<NamedType*>(t)) return true;

  /* All other types */
  return false;

}

bool FieldAccess::isClass()
{
  VarDecl* vd = 0;
  Decl* lookup = 0;
  Type *t = 0;
  NamedType* nt = 0;

  lookup = emitScope->find(this->getName());
  
  if(!lookup) return false;

  vd = dynamic_cast<VarDecl*>(lookup);

  if(!vd) return false;

  t = vd->getVarType();

  if(!t) return false;

  nt = dynamic_cast<NamedType*>(t);

  if(!nt) return false;

  return nt->isClass();

}

/*check field access expressions*/
bool FieldAccess::checkChildren(Check *c)
{	
  /*expression valid only if all its components are valid*/
  bool base_check = true;
  bool field_check = true;
  bool class_check = false;
  bool outside_scope = false;

  field_check = field->check(c);
  
  Decl* d = c->getDecl(field->getName());
  VarDecl* v = dynamic_cast<VarDecl*>(d);

  /*using a function as a field*/
  if(!v) field_check = false;
  
  if (base) 
    {
      /*base check for this.*/
      This* This_ptr = dynamic_cast<This*>(base);
      base_check = base->check(c);
      
      /*if base is "this"*/
      if(base_check && This_ptr)
	{
	  /*get the class of the current scope*/
	  ClassDecl* cd = c->getClassScope();
	  /*if we are in a class*/
	  if(cd)
	    {
	      /*find the field name and check if it is var or fn*/
	      Decl* d = cd->findInClass(field->getName());
	      if(d)
		{
		  //store class pointer
		  myClass = cd;

		  if(d->isFnDecl())
		    {
		      this->ExprType = 
			(dynamic_cast<FnDecl*>(d))->getReturn();
		      return true;
		    }

		  if(d->isVarDecl())
		    {
		      this->ExprType =
			(dynamic_cast<VarDecl*>(d))->getVarType();
		      return true;			
		    }
		} else {
		/*field does not exists in this class*/
		ExprType = Type::errorType;
		ReportError::IdentifierNotDeclared(field, LookingForVariable);
	      }
	      
	    }
	  return false;
	}

      //fix stuff like (3+3).a
      assert(base->ExprType);
      if(!base->ExprType->isNamed())
	{
	  ExprType = Type::errorType;
	  //is this the correct error?
	  ReportError::FieldNotFoundInBase(field, base->ExprType);
	  return false;
	}
      
      /*if base is a normal vardecl*/
      VarDecl* var_decl = dynamic_cast<VarDecl*>
	(c->getDecl(base->getName()));
      
      if(var_decl)
	{

	  ClassDecl* cd = (var_decl->getClass());
	  if(cd)
	    {
	      myClass = cd;
	      /*variable is class and we've found 
	       *a variable (its private)*/
	      if(cd->findMember(field->getName()))
		{

		  outside_scope = !c->inClass(); 
		  ExprType =  outside_scope ? 
		    Type::errorType :
		    (dynamic_cast<VarDecl*>(d))->getVarType();
		  class_check = !outside_scope;
		}
	    } 
	}
    }
  
  /*class check failed*/
  if(base && !class_check)
    {
      /* if we have a base type but its not a class */
      if(outside_scope)
	{
	  assert(base->ExprType);
	  ReportError::InaccessibleField(field, base->ExprType);
	}
      else
	{
	  assert(base->ExprType);
	  ReportError::FieldNotFoundInBase(field, base->ExprType);
	  base_check = false;
	}
      ExprType = Type::errorType;
    }

  if(!field_check && (!(base)))
    {
      ExprType = Type::errorType;
      ReportError::IdentifierNotDeclared(field, LookingForVariable);
    }


  if(field_check && !(base))
    {
      ExprType = myType(c);
      return true;
    }

  else if(field_check && base)
    {

      return true;
    }

  else
    {	       
      return false;
    }
}
/*helper function for field access check
 *retur he type of this class expression*/
Type* FieldAccess::myType(Check *c)
{
  return c->getType(this->getName());
}


Call::Call(yyltype loc, Expr *b, Identifier *f, List<Expr*> *a) : Expr(loc)  {
  assert(f != NULL && a != NULL); 
  // b can be be NULL (just means no explicit base)
  base = b;
  if (base) base->setParent(this);
  (field=f)->setParent(this);
  (actuals=a)->setParentAll(this);
}

void Call::printChildren(int indentLevel) {
  if (base) base->print(indentLevel+1);
  field->print(indentLevel+1);
  actuals->printAll(indentLevel+1, "(actuals) ");
}

/* PP4: Generate TAC for a function/method call */
void Call::emit()
{
  if(base)
    {
      base->emit();
      /* Arrays actually have size = arraysize + 1
       * because the 0th position is the array
       * size */
      if(base->ExprType->isArray())
	{
	/* Implicit length method for arrays */
	  if(strcmp(field->getName(), "length") == 0)
	  {
	    myLocation =  cg->GenLoad(base->GetLocation(), 0);
	  }
	}

      /* Built in GetByte functional,
	 enabled with "byte"on command-line */
      if(enable_byte && 
	 strcmp(field->getName(), "GetByte") == 0)
	{
	  Location *byteOffset = NULL;
	  assert(actuals->size() == 1);

	  actuals->get(0)->emit();

	  byteOffset = actuals->get(0)->GetLocation();

	  assert(byteOffset);

	  myLocation = cg->GenBuiltInCall(GetByte,base->GetLocation(),
					  byteOffset);

	}

      if(enable_byte && 
	 strcmp(field->getName(), "SetByte") == 0)
	{
	  Location *byteOffset = 0;
	  Location *byteValue = 0;

	  assert(actuals->size() == 2);

	  actuals->get(0)->emit();
	  actuals->get(1)->emit();

	  byteOffset = actuals->get(0)->GetLocation();
	  byteValue = actuals->get(1)->GetLocation();

	  assert(byteOffset);
	  assert(byteValue);

	  myLocation = cg->GenSetByte(base->GetLocation(),
				      byteOffset,
				      byteValue);

	}



      /* Method call */
      else if(base->ExprType->isNamed())
      {
	int popSize = 0;
	Decl *d = 0;
	ClassDecl *cd = 0;
	Location *vptr_addr = 0;
	int method_offset = -1;
	Location *method_addr = 0;

	actuals->emitAll();

	/* Push arguments onto the stack */
	for(int i = actuals->size()-1; i>=0; i--)
	  {
	    actuals->get(i)->PushOntoStack();
	    popSize += 4;
	  }

	cg->GenPushParam(base->GetLocation());
	popSize += 4;

	/* Search for class declaration, needed
	 * to search vtable for method pointer */
	d = emitScope->find(base->ExprType->getType());
	cd = dynamic_cast<ClassDecl*>(d);

	assert(cd);

	/* Push "this" onto the stack */
	vptr_addr = cg->GenLoad(base->GetLocation());
	
	/* Find method offset with in the current 
	 * vtable */
	method_offset = cd->findMethodOffset(field->getName());

	/* Create reference to the method's address with
	 * the vtable for the class in question */
	method_addr = cg->GenLoad(vptr_addr,method_offset);
       	
	/* Generate a address call TAC instruction ... aka
	 * a MIPS "jal" instruction */
	myLocation = cg->GenACall(method_addr, true);

	/* Pop everything we've pushed on the stack*/
	cg->GenPopParams(popSize);
	
      }
    } 
  
  /* Old fashioned function calls and method calls
   * with an implicit this infront of them */
  else 
    {
      int popsize = 0;
      FnDecl *temp_fn = 0;
      char *field_name = 0;
      ClassDecl *cd = 0;
      Location *vptr_addr = 0;
      int method_offset = -1;
      Location *method_addr = 0;
      char* codeLabel = 0;

      actuals->emitAll();

      field_name = field->getName();

      temp_fn =
	dynamic_cast<FnDecl*>(emitScope->find(field_name));
      
      /*detect implicit this by checking if parent is a classdecl
       *shadowed variables will not have a classdecl as a parent
       *boo ya: syntax directed translation in da house!
       */
      cd = dynamic_cast<ClassDecl*>(temp_fn->getParent());

      /* Generate a method call */
      if(cd)
	{
	  /* Push arguments onto the stack */
	  for(int i = actuals->size()-1; i>=0; i--)
	    {	
	      actuals->get(i)->PushOntoStack();
	      popsize += 4;
	    }

	  /* push "this" */
	  cg->GenPushParam(cg->getThis());
	  popsize += 4;

	  /* Calculate offset */
	  vptr_addr = cg->GenLoad(cg->getThis());
	  method_offset = cd->findMethodOffset(field->getName());

	  /* Address of method in vtable */
	  method_addr = cg->GenLoad(vptr_addr,method_offset);
	  myLocation = cg->GenACall(method_addr, true);
	  cg->GenPopParams(popsize);
	}
      /* Generate a function call */
      else 
	{
	
	  for(int i = actuals->size()-1; i>=0; i--)
	    {	 
	      actuals->get(i)->PushOntoStack();
	      popsize += 4;
	    }
	  
	  codeLabel = new char[1 + strlen(field->getName())+1]; 
	
	  /* Prepend a "_" before function names to avoid 
	   * generating labels with the same name as 
	   * MIPS opcodes (a known bug in SPIM) */
	  if(strcmp(field->getName(),"main")==0)
	    {
	      /* Main function has to be called
	       * "main"; otherwise SPIM can't 
	       * find it. No MIPS instruction
	       * has the opcode main, so it's cool
	       */
	      sprintf(codeLabel, "%s",field->getName());
	    } 
	  else 
	    {
	      /* Print method name with "_" */
	      sprintf(codeLabel, "_%s",field->getName());
	    }
	  
	  this->myLocation = cg->GenLCall(codeLabel,true);
	  cg->GenPopParams(popsize);
	}
    }
}


/*check call experssions*/
bool Call::checkChildren(Check *c)
{
  /*expression valid only if all its sub exressions are valid*/
  bool base_check = true;
  bool field_check = true;
  bool actuals_check = true;

  if (base) base_check = base->check(c);
  field_check = field->check(c);
  actuals_check = actuals->checkAll(c);

  Decl* field_decl = static_cast<Decl*>(NULL);
  field_decl = c->getDecl(field->getName());

  FnDecl* fn_decl = static_cast<FnDecl*>(NULL);

  if(field_decl)
    {
      fn_decl = dynamic_cast<FnDecl*>(field_decl);
    }

  /*field declaration is invalid*/
  //if(!fn_decl && !base)
  // {
  //   ExprType = Type::errorType;
  //   return true;
  // }

  /*if there is a base*/
  if(base)
    {
      assert(base->ExprType);

      if(base->ExprType->isArray())
	{
	  if(strcmp(field->getName(), "length") == 0)
	    { 
	      ExprType = Type::intType;
	      return true;
	    }
	  
	}
      /* PP4 Optional */
      if((base->ExprType == Type::stringType) &&
	 enable_byte)
	{
	  if(strcmp(field->getName(), "getByte") == 0)
	    { 
	      ExprType = Type::intType;
	      return true;
	    }
	 else if(strcmp(field->getName(), "setByte") == 0)
	    { 
	      ExprType = Type::voidType;
	      return true;
	    }
	}


      Type* myType = base->ExprType;

      if(myType->isNamed())
	{
	  ClassDecl *cd = dynamic_cast<ClassDecl*>
	    (c->getDecl(myType->getType()));
	  /*look for named class*/ 
	  if(cd) 
	    {	     
	      /*look for method within the class*/
	      FnDecl *fd = cd->findMethod(field->getName());
	      if(fd)
		{
		  /*make sure the parameters matches the method*/
		  bool match = fd->validParameters(actuals,field,c);
		  this->ExprType = match ? fd->getReturn() :
		    Type::errorType;
		  return match;
		}
	      else
		{
		  ReportError::FieldNotFoundInBase(field,base->ExprType);
		  this->ExprType = Type::errorType;
		  return false;
		}
	    }
	  /*other wise, base could be an interface, and we look
	    for interface methods*/

	  InterfaceDecl *id =
	    dynamic_cast<InterfaceDecl*>(c->getDecl(myType->getType()));
	  if(id) 
	    {
	      FnDecl *fd = id->findMethod(field->getName());
	      if(fd)
		{
		  bool match = fd->validParameters(actuals,field,c);
		  this->ExprType = match ? fd->getReturn() :
		    Type::errorType;
		  
		  return match;
		}
	      else
		{
		  ReportError::FieldNotFoundInBase(field,base->ExprType);
		  this->ExprType = Type::errorType;
		  return false;
		}
	    }
	}

    }

 
  if(base && !field_check)
    {
      assert(base->ExprType);

      if(base->ExprType==Type::errorType)
	return true;

      ReportError::FieldNotFoundInBase(field,base->ExprType);
      ExprType = Type::errorType;
      return false;
    }

  if(!field_check)
    {
      // printf("!field_check line, this = %p\n", this);
      ReportError::IdentifierNotDeclared(field, LookingForFunction); 
    }
  else if(field_check && !fn_decl)
    {
      // printf("field_check && !fn_decl this = %p\n", this);
      ReportError::IdentifierNotDeclared(field, LookingForFunction);
    }
  

  if(base_check && field_check && actuals_check)
    {
      bool match = fn_decl->validParameters(actuals,field,c);
      ExprType = match ? fn_decl->getReturn() : Type::errorType;
      return match;
    }
  else
    { 
      ExprType = Type::errorType;
      return false;
    }
}

SetByte::SetByte(yyltype loc, Expr *b, Expr* ex1, Expr* ex2) : Expr(loc)  {
  assert( b!=NULL && ex1!=NULL && ex2!=NULL); 
  base = b;
  if (base) base->setParent(this);
  (e1=ex1)->setParent(this);
  (e2=ex2)->setParent(this);
}

void SetByte::printChildren(int indentLevel) {
  if (base) base->print(indentLevel+1);
  e1->print(indentLevel+1, "(Expr 1) ");
  e2->print(indentLevel+1, "(Expr 2) ");
}

bool SetByte::checkChildren(Check *c){

  bool check_base = base->check(c);
  bool check_e1 = e1->check(c);
  bool check_e2 = e2->check(c);
  /*ignore parameter erros if base does not check out*/
  if(check_base && (base->ExprType!=Type::errorType)){
    /*make sure all exprs are ints and aren't nulls*/
    if(e1->ExprType!=NULL){
      check_e1 = (e1->ExprType == Type::intType);
      if(!check_e1 && (e1->ExprType!=Type::errorType)){
	ReportError::ArgMismatch(e1,1,e1->ExprType,Type::intType);
      }
    } else {
      ReportError::ArgMismatch(e1,1,Type::nullType,Type::intType);
    }
    if(e2->ExprType!=NULL){
      check_e2 = (e2->ExprType == Type::intType);
      if(!check_e2 && (e2->ExprType!=Type::errorType)){
	ReportError::ArgMismatch(e2,1,e2->ExprType,Type::intType);
      }
    } else {
      ReportError::ArgMismatch(e2,1,Type::nullType,Type::intType);
    }
  }
  this->ExprType = Type::voidType;
  return check_base&&check_e1&&check_e2;
}

GetByte::GetByte(yyltype loc, Expr *b, Expr* ex1) : Expr(loc)  {
  assert(b!=NULL && ex1!=NULL); 
  base = b;
  if (base) base->setParent(this);
  (e1=ex1)->setParent(this);
}

void GetByte::printChildren(int indentLevel) {
  if (base) base->print(indentLevel+1);
  e1->print(indentLevel+1, "(Expr 1) ");
}

bool GetByte::checkChildren(Check *c){
  bool check_base = base->check(c);
  bool check_e1 = e1->check(c);  
  /*ignore parameter erros if base does not check out*/
  if(check_base && (base->ExprType!=Type::errorType)){
    /*make sure all exprs are ints */
    check_e1 = (e1->ExprType == Type::intType);
    if(!check_e1 && (e1->ExprType!=Type::errorType)){
      ReportError::ArgMismatch(e1,1,e1->ExprType,Type::intType);
    }
  }
  this->ExprType = Type::intType;
  return check_base&&check_e1;
}

NewExpr::NewExpr(yyltype loc, NamedType *c) : Expr(loc) {
  assert(c != NULL);
  (cType=c)->setParent(this);
}

void NewExpr::printChildren(int indentLevel) {	
  cType->print(indentLevel+1);
}

/* PP4: Emit TAC for a NewExpr */
void NewExpr::emit(){
  char* className = 0;
  char* modifiedLabel = 0;
  int classLen = 0; 
  
  Decl* this_type = emitScope->find(cType->getType());
  ClassDecl* this_class = dynamic_cast<ClassDecl*>(this_type);
  Location *alloc_size =
    cg->GenLoadConstant(this_class->getVariableOffset());
  Location *new_mem= cg->GenBuiltInCall(Alloc,alloc_size ,NULL);

  className = this_class->getName();
  classLen = strlen(className);

  modifiedLabel = new char[classLen + 2];
  snprintf(modifiedLabel, classLen + 2, "_%s", className);
  

  Location *vptr = cg->GenLoadLabel(modifiedLabel);


  cg->GenStore(new_mem, vptr);
  myLocation = new_mem;

}

/*check the "new " expressions*/
bool NewExpr::checkChildren(Check *c)
{
  /*if there is an error, we must print out "class" instead of "type"
   *option lets type.check() print the appropriate message*/
  c->setOption(0);
  
  if(cType->check(c) && cType->isClass())
    {
      this->ExprType = cType;
      return true;
    }
  else
    {
      /*the expression new can only be applied to clases and no interface*/
      if(cType->isInterface()){
	ReportError::IdentifierNotDeclared
	  (cType->getId(), LookingForClass);
      }
      this->ExprType = Type::errorType;
      return false;
    }
}

NewArrayExpr::NewArrayExpr(yyltype loc, Expr *sz, Type *et) : Expr(loc) {
  assert(sz != NULL && et != NULL);
  (size=sz)->setParent(this);
  (elemType=et)->setParent(this);
}

void NewArrayExpr::printChildren(int indentLevel) {
  size->print(indentLevel+1);
  elemType->print(indentLevel+1);
}

/* PP4: Emit TAC for NewArray()
 * Arrays are arraysize+1 to store the size of the array
 */
void NewArrayExpr::emit(){
  size->emit();
  char *not_neg = cg->NewLabel();
  Location *zero = cg->GenLoadConstant(0);
  Location *notvalid = cg->GenBinaryOp("<",size->GetLocation(),zero);
  cg->GenIfZ(notvalid,not_neg);
  Location *error_message 
    = cg->GenLoadConstant("Decaf runtime error: Array size is <= 0");
  cg->GenBuiltInCall(PrintString, error_message,NULL);
  cg->GenBuiltInCall(Halt,NULL,NULL);
  cg->GenLabel(not_neg);
  Location *one = cg->GenLoadConstant(1);
  Location *four=  cg->GenLoadConstant(4);
  Location *realsize = cg->GenBinaryOp("+",size->GetLocation(),one);
  Location *alloc_size = cg->GenBinaryOp("*",realsize,four);
  this->myLocation = cg->GenBuiltInCall(Alloc,alloc_size,NULL);
  cg->GenStore(this->myLocation,size->GetLocation(),0);
}
/*check new array expressions*/
bool NewArrayExpr::checkChildren(Check *c)
{
  /*expression is true only when its sub expressions are true*/
  bool size_check = true;
  bool elemType_check = true;

  size_check =size->check(c);

  if(elemType->isNamed())
    {
      /*print "type" instead of "class" in the error message*/
      c->resetOption();
      elemType_check = elemType->check(c);
    }


  /*check that new array size is an integer*/
  if(size->ExprType != Type::intType)
    {
      ExprType = Type::errorType;
      ReportError::NewArraySizeNotInteger(size);
      size_check = false;
    } else {
    ExprType =  new ArrayType((*size->getLocation()),elemType);
  }

  
  return size_check && elemType_check;
}


MallocExpr::MallocExpr(yyltype loc, Expr *sz) : Expr(loc) {
  assert(sz != NULL);
  (size=sz)->setParent(this);
}

void MallocExpr::printChildren(int indentLevel) {	
  size->print(indentLevel+1);
}

//pp4
void MallocExpr::emit(){
  size->emit();
  this->myLocation = cg->GenBuiltInCall(Alloc,size->GetLocation(),NULL);

}

/*check the malloc expressions*/
bool MallocExpr::checkChildren(Check *c) 
{	
  bool size_check = size->check(c);
  
  //if expr is correct, check to make sure it is inttype
  if(size_check){
    size_check = (size->ExprType==Type::intType);
    if(size_check)
      {
	this->ExprType = Type::specialType;
      } else {
      ReportError::ArgMismatch(size,1,size->ExprType,Type::intType);
    }
  } 
 
  /*any error will result in errortype*/
  if(!size_check){
    this->ExprType = Type::errorType;
  }

  return size_check;
}

/* PP4: Emit TAC for the ReadInteger expression */
void  ReadIntegerExpr::emit(){

  this->myLocation = cg->GenBuiltInCall(ReadInteger,NULL,NULL);
}

/*always returns an integer type*/
bool ReadIntegerExpr::checkChildren(Check *c)
{
  this->ExprType = Type::intType;
  return true;

}

/* PP4: Emit TAC for the ReadLine expression */
void ReadLineExpr::emit(){
  
  this->myLocation = cg->GenBuiltInCall(ReadLine,NULL,NULL);
}

/*always return a string type*/
bool ReadLineExpr::checkChildren(Check *c)
{
  this->ExprType = Type::stringType;
  return true;
}

