/* File: ast_type.cpp */

#include "ast_type.h"
#include "ast_decl.h"
#include "debug.h"
#include "errors.h"
#include <string>
//pp4
#include "codegen.h"

/* Class constants
 *
 * These are public constants for the built-in base types (int, double, etc.)
 * They can be accessed with the syntax Type::intType. This allows you to
 * directly access them and share the built-in types where needed rather that
 * creates lots of copies.
 */

Type *Type::intType    = new Type("int");
Type *Type::doubleType = new Type("double");
Type *Type::voidType   = new Type("void");
Type *Type::boolType   = new Type("bool");
Type *Type::nullType   = new Type("null");
Type *Type::stringType = new Type("string");
Type *Type::errorType  = new Type("error");
/*special type used by malloc*/
Type *Type::specialType = new Type("void*");

Type::Type(const char *n) {
    assert(n);
    typeName = static_cast<char*>(NULL);
    typeName = strdup(n);
}


bool Type::operator==(Type &rhs) const
{
  
  //address comp for base class
  return (this == &rhs);
}

bool Type::operator!=(Type &rhs) const
{
  return !(*this == rhs);
}

//Check if the current type is null. String compare
//instead of address compare.  
bool Type::isNull()
{
  bool is_null = false;
  char* typeStr = static_cast<char*>(NULL);
  
  typeStr = this->getType();

  if(typeStr)
    {
      is_null = (strcmp(typeStr,"null") == 0);
    }

  return is_null;
}

char* Type::getType()
{
  return strdup(typeName);
}

void Type::printChildren(int indentLevel) {
    printf("%s", typeName);
}

bool Type::CompareTypes(Check *c, Type * t0, Type* t1){
  bool match = true;;
  //check for NULL pointers, this shouldn't happen
  if(!t0 || !t1)
    {
      match = false;
      return match;
    }
  
  //Type cast to possible subclass types
  NamedType* rhs_nt = dynamic_cast<NamedType*>(t1);
  NamedType* lhs_nt = dynamic_cast<NamedType*>(t0);
  ArrayType* rhs_at = dynamic_cast<ArrayType*>(t1);
  ArrayType* lhs_at = dynamic_cast<ArrayType*>(t0);
  
  //Attempting to match named type arguments
  if(rhs_nt && lhs_nt)
    {
      match = (*rhs_nt == *lhs_nt);
      
      /* Handle inheritance */
      if(!match)
	{
	  Decl *d = c->getDecl(rhs_nt->getType());
	  ClassDecl* cd = dynamic_cast<ClassDecl*>(d);
	  
	  //extend match
	  while(cd)
	    {
	      //check the implements first
	      List<NamedType *> *implements = cd->getImplementClass();
	      bool check_implement = false;
	      for(int i = 0; i <implements->size();i++)
		{
		  NamedType *it = implements->get(i);
		  if(*it == *lhs_nt)
		    {
		      check_implement = true;
		      break;
		    }
		}
	      if(check_implement)
		{
		  match = true;
		  break;
		}
	      //then check the actual extends which can happen recursively
	      NamedType *nt = cd->getBaseClass();
	      if(*nt == *lhs_nt)
		{
		  match = true;
		  break;
		}
	      
	      d = c->getDecl(nt->getType());
	      cd = dynamic_cast<ClassDecl*>(d);
	    }
	}
    }
  //Check for array type match
  else if(rhs_at && lhs_at)
    match = (*rhs_at == *lhs_at);
  //Elemental types
  else
    {
      match = (*t0 == *t1);  
      //Null check.
      if(strcmp(t0->getType(),"null")==0 && t1->isNamed()){
	match = true;
      }
    }
  /*special check for if program is "return null;"*/
  if(lhs_nt && t1->isNull())
    match= true;;
  
  return match;
}

//Overloaded method used for semantic checking
bool Type::checkChildren(Check *c) 
{
  //dynamically dispatch to proper dervived class
  if(this->isNamed())
    {
      NamedType *nt = dynamic_cast<NamedType*>(this);
      assert(nt);
      return nt->check(c);
    }
  else if(this->isArray())
    {
      ArrayType *at = dynamic_cast<ArrayType*>(this);
      assert(at);
      return at->check(c);
    }
  else
    {
      //int,bool,double,string,etc
      return true;
    }
}


char* NamedType::getType()
{
  return strdup(id->getName());
}

bool NamedType::operator==(NamedType &rhs) const
{
  char* myName = NULL;
  char* otherName = NULL;
  bool  valid_test = true;

  assert(this != static_cast<NamedType*>(NULL));

  if(this->id != static_cast<Identifier*>(NULL))
    myName = this->id->getName();
  
  if(rhs.id != static_cast<Identifier*>(NULL))
    otherName = rhs.id->getName();
  
  if(!myName || !otherName)
    return false;
  else
    {
      valid_test = (strcmp(myName,otherName) == 0);
      return valid_test;
    }
}

bool NamedType::operator!=(NamedType &rhs) const
{
  return !(*this == rhs);
}

	
NamedType::NamedType(Identifier *i) : Type(*i->getLocation()) {
    assert(i != NULL);
    (id=i)->setParent(this);
}

void NamedType::printChildren(int indentLevel) {
    id->print(indentLevel+1);
}

//dsheffie:
bool NamedType::checkChildren(Check *c)
{
  bool valid_type = false;
  Decl* d = c->getDecl(id->getName());
  isClassType = false;
  isInterfaceType = false;
  
  //Search if named variable has already been declared.
  if(!d) 
    {
      //If we can't find a reference for the variable name
      //the NamedType is obviously invalid
      valid_type = false;
    }
  else
    {
      //NamedTypes are either interfaces or classes.
      ClassDecl* cd = dynamic_cast<ClassDecl*>(d);
      if(cd) 
	{
	  valid_type = true;
	  isClassType = true;
	}
      InterfaceDecl* id = dynamic_cast<InterfaceDecl*>(d);
      if(id)
	{
	  valid_type = true;
	  isInterfaceType = true;
	}
    }

  //newexpr require we print "class" instead of "type" in the error statement
  if(!valid_type)
    {
      //store search context in the Check obj
      if(c->getOption() == 0)
	{
	  c->resetOption();
	  ReportError::IdentifierNotDeclared
	    (this->getId(), LookingForClass);
	} else {
	ReportError::IdentifierNotDeclared
	  (this->getId(), LookingForType);
      }
    }


  return valid_type;
}

bool ArrayType::operator==(ArrayType &rhs) const
{
  ArrayType* at0 = dynamic_cast<ArrayType*>(this->elemType);
  ArrayType* at1 = dynamic_cast<ArrayType*>(rhs.elemType);

  NamedType *nt0 = dynamic_cast<NamedType*>(this->elemType);
  NamedType *nt1 = dynamic_cast<NamedType*>(rhs.elemType);

  //check for int[][] == int[][] 
  if(at0 && at1)
    {
      return (*at0 == *at1);
    }

  else if(nt0 && nt1)
    {
      return (*nt0 == *nt1);
    }
  //check for int[] == int[][] issues
  else if((at0 && !at1) || (!at0 && at1))
    {
      return false;
    }
  else
    {
      //compare elemental types when arr[] == arr[]
      return(*(this->elemType) == *(rhs.elemType));
    }
}

bool ArrayType::operator!=(ArrayType &rhs) const
{
  return !(*this == rhs);
}

ArrayType::ArrayType(yyltype loc, Type *et) : Type(loc) {
    assert(et != NULL);
    (elemType=et)->setParent(this);
}

void ArrayType::printChildren(int indentLevel) {
    elemType->print(indentLevel+1);
}


//dsheffie:
bool ArrayType::checkChildren(Check *c)
{
  return elemType->check(c);
}

bool ArrayType::IsEquivalentTo(Type *other)
{
  return (other == this->elemType);
}


char* ArrayType::getType()
{
  return elemType->getType();
}
