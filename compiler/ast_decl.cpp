/* File: ast_decl.cpp */

#include "ast_decl.h"
#include "ast_type.h"
#include "ast_stmt.h"
#include "ast_expr.h"
#include "debug.h"
//pp4
#include "codegen.h"

//hack
Check * global_c;
extern bool enable_float;

Decl::Decl(Identifier *n) : Node(*n->getLocation()) {
  assert(n != NULL);
  (id=n)->setParent(this);
}

char* Decl::getName()
{
  char* n = NULL;
  n = (id != NULL) ?  id->getName() : NULL;
  return n;
}

//pp4
void VarDecl::emit()
{
  //if emit scope is null means we are on the global scope and 
  //we generate global variables instead of temporary
  if(emitScope ==NULL)
    {
      this->myLocation = cg->GenGlobalVar(this->getName());

      if(enable_float && (this->type == Type::doubleType))
	{
	  this->myLocation->SetFloat(true);
	}

    }
  else 
    {
      this->myLocation = cg->GenTempVar(this->getName());

      if(enable_float && (this->type == Type::doubleType))
	{
	  this->myLocation->SetFloat(true);
	}
    }
}

bool VarDecl::operator!=(const VarDecl &rhs) const
{
  return !(*this == rhs);
  
}

/* Variable declarations are compared by name,
 * type, and if they are class declarations */
bool VarDecl::operator==(const VarDecl &rhs) const
{
  bool class_check = true;
  bool type_check = true;
  bool name_check = true;

  class_check = (this->isClass == rhs.isClass);
  type_check = strcmp(this->type->getType(), rhs.type->getType())==0;
  name_check = (*(this->id) == *(rhs.id));
  return (class_check && type_check && name_check);
}


VarDecl::VarDecl(Identifier *n, Type *t) : Decl(n) {
  assert(n != NULL && t != NULL);
  (type=t)->setParent(this);
  //Initialize class to NULL
  this->myClass = static_cast<ClassDecl*>(NULL);
  this->isClass = false;
}

void VarDecl::printChildren(int indentLevel) {
  type->print(indentLevel+1);
  id->print(indentLevel+1);
}

/* Check if the VarDecl is contained in a class */
bool VarDecl::checkClass()
{
  return (myClass);
}

/* Variable declarations can be attached to ClassDecl */
void VarDecl::setClass(ClassDecl *myClass)
{
  this->myClass = myClass;
}


/* Semantic checking for variable declarations */
bool VarDecl::checkChildren(Check *c) 
{
  bool type_check = true;
  bool id_check = true;

  /* First: check the semantic validity of the
   * declared type and name (id) */
  type_check = type->check(c);
  id_check = id->check(c);
  

  /* If the type referes to a class, we need
   * get a reference to the class declaration
   */
  Decl* d = c->getDecl(type->getType());
  ClassDecl *cd = dynamic_cast<ClassDecl*>(d);

  //attach class declaration
  if(cd) setClass(cd);
  
  if(type_check == true && id_check == true)
    {
      /* If adding fails, it means we've already added a variable
       * with the same name to the current scope */
      if(!c->addDecl(this))
	{
	  ReportError::DeclConflict(this, c->getDecl(this->getName()));
	  return false;
	}

             
      ClassDecl* myClass = dynamic_cast<ClassDecl*>(parent);
      if(myClass)
	{
	  myClass->addToClassScope(this);
	  // myClass->addMember(this);
	}
      return true;
    }
  else 
    {
      //Invalid type, allow forward error checking
      if(id_check==true)
	{
	  type = Type::errorType;
	  /* Namespace collision in current scope */
	  if(!c->addDecl(this))
	    {
	      ReportError::DeclConflict(this, c->getDecl(this->getName()));
	      return false;
	    }
	  
	  //Attach to parent class (if present)
	  ClassDecl* myClass = dynamic_cast<ClassDecl*>(parent);
	  if(myClass)
	    {
	      myClass->addToClassScope(this);
	      //myClass->addMember(this);
	    }
	}    
      return false;
    }
  
}

void ClassDecl::makeVTable(llvm::LLVMContext *Context,
			   llvm::Module *myModule,
			   llvm::IRBuilder<> *myIRBuilder,
			   std::map<std::string, llvm::Value*> &labelMap,
			   std::map<std::string, std::vector<std::string> > &vtableNames
			   ) {
  llvm::GlobalVariable *gV = nullptr;
  llvm::Type *iType32 = llvm::Type::getInt32Ty(*Context);
  int nEntries = methods->size();
  llvm::VectorType *vType32 = llvm::VectorType::get(iType32, nEntries);

  std::string vName = std::string("_") + std::string(getName());
  gV = new llvm::GlobalVariable(*myModule,
				vType32,
				false,
				llvm::GlobalValue::PrivateLinkage,//llvm::GlobalValue::LinkerPrivateLinkage,
				llvm::Constant::getNullValue(vType32),
				vName.c_str());
  labelMap[vName] = gV;
  std::vector<std::string> methodNames;
  for(int i = 0, len = methods->size(); i <len; i++) {
      FnDecl* m = methods->get(i);
      ClassDecl* parent_class = dynamic_cast<ClassDecl*>(m->getParent());
      int strClassLen = strlen(parent_class->getName());
      char* method_label = 0;
      char* strMethod = m->getName();
      int strMethodLen = strlen(strMethod);
      
      int bytesToAlloc = 1 + strClassLen + 1 + strMethodLen + 1;
      method_label = new char[bytesToAlloc+1]; 
      bzero(method_label, bytesToAlloc+1);
      
  
      snprintf(method_label,bytesToAlloc, "_%s.%s",parent_class->getName(), 
	       strMethod);
      methodNames.push_back(std::string(method_label));
    }
  vtableNames[vName] = methodNames;
}

void ClassDecl::genVTable()
{
  List<const char*> *methodLabels;
  ClassVarOffsets = new Hashtable<int>;
  addVariableOffset("*vptr");
  //add other variable offsets
  for(int i = 0; i < member_decls->size(); i++)
    {
      VarDecl *v = member_decls->get(i);
      addVariableOffset(v->getName());
      
    }
  methodLabels = new List<const char*>;
  MethodOffsets = new Hashtable<int>;
  
  char* strClass = this->getName(); 
  assert(strClass);
  int classLen = strlen(strClass);
  char* vTblName = 0;


  //collection function labels
  for(int i = 0; i < methods->size(); i++)
    {
      FnDecl* m = methods->get(i);
      ClassDecl* parent_class = dynamic_cast<ClassDecl*>(m->getParent());
      int strClassLen = strlen(parent_class->getName());
      char* method_label = 0;
      char* strMethod = m->getName();
      int strMethodLen = strlen(strMethod);
      
      int bytesToAlloc = 1 + strClassLen + 1 + strMethodLen + 1;
      method_label = new char[bytesToAlloc+1]; 
      bzero(method_label, bytesToAlloc+1);
      
  
      snprintf(method_label,bytesToAlloc, "_%s.%s",parent_class->getName(), 
	       strMethod);

      methodLabels->append(method_label);

      MethodOffsets->put(strMethod,i*4);
    }
  
  vTblName = new char[classLen + 2];
  bzero(vTblName,(classLen + 2));



  snprintf(vTblName, (classLen + 2), "_%s", strClass);
  
 
  cg->GenVTable(vTblName, methodLabels);
}


int ClassDecl::findMethodOffset(char *methodName)
{

  int location =  -1;
  location = MethodOffsets->get(methodName);
  
  
  return location;
}


//pp4 
void ClassDecl::emit()
{
  Scope *tempScope = emitScope;
  List<const char*> *methodLabels;
  
  emitScope = this->myScope;
 
  for(int i = 0; i<methods->size(); i++){

    ClassDecl* parent_class 
      = dynamic_cast<ClassDecl*>(methods->get(i)->getParent());
    if(parent_class==this){
      methods->get(i)->emit();
    }
  }
		
  //Need to handle extend in the future

  //or do we....


  emitScope = tempScope;
		
 
}

void ClassDecl::addVariableOffset(char* memberName)
{
  ClassVarOffsets->put(memberName,currentOffset);
  currentOffset = currentOffset + 4;
 
}

int ClassDecl::findVariableOffset(char *memberName)
{
  
  int location =  -1;
  location = ClassVarOffsets->get(memberName);


  //location 0 is the vtpr...this shouldn't ever occur
  assert(location != 0);


  if(location == 0) return -1;
  else return location;

}

int ClassDecl::getVariableOffset(){
  return currentOffset;
}


ClassDecl::ClassDecl(Identifier *n, NamedType *ex, List<NamedType*> *imp, 
		     List<Decl*> *m) : Decl(n) 
{
  // extends can be NULL, impl & mem may be empty lists but cannot be NULL
  assert(n != NULL && imp != NULL && m != NULL);
  extends = ex;
  if (extends) extends->setParent(this);
  (implements=imp)->setParentAll(this);
  (members=m)->setParentAll(this);


  /* Augmented the constructor with a couple of 
   * fields needed for semantic checking */

  //Scope within the class
  class_scope = new Scope();
    
  //List of interface declarations references the class needs to implement
  interfaces_to_implement = new List<InterfaceDecl*>;
  //References to methods
  methods = new List<FnDecl*>;
  //References to variables
  member_decls = new List<VarDecl*>;

  currentOffset = 0;
  
}

/* Add a declaration to the private scope
 * contained in a class declaration. */
void ClassDecl::addToClassScope(Decl* d)
{
  //prevent the double declarations of methods and variables in a class
  Decl* temp_check = class_scope->find(d->getName());
  if(temp_check!=static_cast<Decl*>(NULL)){
  } else {
    class_scope->add(d->getName(), d, true);
  }
}


//Search for a declaration (any type) within a class
Decl* ClassDecl::findInClass(char* name)
{
  return class_scope->find(name);
}

Scope* ClassDecl::getScope()
{
  Scope *n = new Scope;
  n->Copy(class_scope);
  
  return n;
}


//Search a class for a method declaration
FnDecl* ClassDecl::findMethod(char* name)
{
  return dynamic_cast<FnDecl*>(class_scope->find(name));
}

//Search a class scope for a variable declaration
VarDecl* ClassDecl::findMember(char* name)
{
  // class_scope->printScope();
  return dynamic_cast<VarDecl*>(class_scope->find(name));
}

void ClassDecl::printChildren(int indentLevel) {
  id->print(indentLevel+1);
  if (extends) extends->print(indentLevel+1, "(extends) ");
  implements->printAll(indentLevel+1, "(implements) ");
  members->printAll(indentLevel+1);
}

/* Copy the entire scope from class into another 
 * class.  This is used when copying all the
 * members from a base class into a extended
 * class */
void ClassDecl::implement_class(InterfaceDecl* base)
{
  //Not needed in PP4

  //  if(base){
  //     List<Decl*> *ifmember = base->getMembers();
  //     for(int i = 0; i < ifmember->size(); i++)
  //       {
  // 	if(ifmember->get(i)->isFnDecl()) {
  // 	  FnDecl* base_method = dynamic_cast<FnDecl*>(ifmember->get(i));
  // 	  this->methods->append(base_method);
  // 	} else {
  // 	  VarDecl* base_member = dynamic_cast<VarDecl*>(ifmember->get(i));
  // 	  this->member_decls->append(base_member);
  // 	}
  //       }
  //   }

}


/* Copy the entire scope from class into another 
 * class.  This is used when copying all the
 * members from a base class into a extended
 * class */
void ClassDecl::extend_class(ClassDecl* base,Check* c)
{
  if(base)
    {
      if(base->getBaseClass())
	{
	  ClassDecl *base_base =
	    dynamic_cast<ClassDecl*>(c->getDecl(base->getBaseClass()->getType()));
	  extend_class(base_base,c);
	}
      List<Decl*> *ifmember = base->getMembers();
      for(int i = 0; i < ifmember->size(); i++)
	{
	if(ifmember->get(i)->isFnDecl()) 
	  {
	    bool added = false;
	    FnDecl* base_method = dynamic_cast<FnDecl*>(ifmember->get(i));
	    for(int j = 0; j<methods->size();j++)
	      {
		if(strcmp(base_method->getName(), this->methods->get(j)->getName())==0){
		  this->methods->remove(j);
		  this->methods->insert(base_method,j);
		  added = true;
		}
	      }
	    if(!added)
	      {
		this->methods->append(base_method);
	      }
	  } 
	else 
	  {
	    VarDecl* base_member = dynamic_cast<VarDecl*>(ifmember->get(i));
	    this->member_decls->append(base_member);
	  }
	}
    }
}


/* In order to handle forward declarations,
 * we add all the ClassDecl signatures
 * to the current scope before checking
 * for validity */
void ClassDecl::getSignature(Check *c)
{ 
  if(!c->addDecl(this)){
    ReportError::DeclConflict(this, c->getDecl(this->getName()));
    return;
  }
  for(int i = 0;  i < members->size(); i++)
    {
      Decl* d = members->get(i);
      FnDecl *fd = dynamic_cast<FnDecl*>(d);
      VarDecl *vd = dynamic_cast<VarDecl*>(d);
      if(fd)
	{
	  addToClassScope(fd);
	}
      if(vd){
	addToClassScope(vd);
      }
    }

}

/* The semantic checking method for a 
 * ClassDecl */
bool ClassDecl::checkChildren(Check *c) 
{
  bool id_check = true;
  bool extends_check = true;
  bool implements_check = true;
  bool members_check = true;
  ClassDecl* cd = static_cast<ClassDecl*>(NULL);
  c->setClassScope(this);
  id_check = id->check(c);

  //Check if the extends field is valid
  if (extends) 
    {
      //find class declaration for "extends"
      Decl* d = c->getDecl(extends->getType());
      cd = dynamic_cast<ClassDecl*>(d);
     
      if(!cd)
	{
	  //No description in scope for the desired class
	  ReportError::IdentifierNotDeclared
	    (extends->getId(), LookingForClass); 
	  extends_check = false;
	  
	} 
      else 
	{
	  //Found the class, make sure it's valid
	  extends_check = extends->check(c);
	  if(extends_check)
	    {
	      for(int i = 0; i<members->size(); i++)
		{
		  Decl* decl = this->members->get(i);
		  //printf("looking for %s\n", decl->getName());
		  //make sure there is no function and variable declaration conflicts
		  if(dynamic_cast<FnDecl*>(decl)!=NULL)
		    {
		      Decl* temp = cd->findInClass(decl->getName());
		      if(temp!=NULL)
			{
			  FnDecl *lol = dynamic_cast<FnDecl*>(decl);
			  FnDecl *z = 	dynamic_cast<FnDecl*>(temp);	 
			  if((*lol)!=(*z))
			    {
			      ReportError::OverrideMismatch(decl);
			    }
			}
		    }
		  else if(dynamic_cast<VarDecl*>(decl)!=NULL)
		    {
		      Decl* temp = cd->findInClass(decl->getName());
		      if(dynamic_cast<VarDecl*>(temp)!=static_cast<VarDecl*>(NULL))
			{
			  ReportError::DeclConflict(decl,temp);
			}
		    }
		}
	    }
	}
      
    }
   
  //Check implememts for validity
  if(implements)
    {
      //Iterate over all interfaces to implement
      for(int i = 0; i < implements->size(); i++)
	{
	  //Search for the interface declaration in the current
	  //scope.
	  char* interface_name = implements->get(i)->getType();
	  Identifier* implements_id = implements->get(i)->getId();
	  bool found_decl = false;
	  assert(interface_name);
	  Decl *d = c->getDecl(interface_name);
	  found_decl = (d != static_cast<Decl*>(NULL));
	  
	 
	  if(!found_decl)
	    {
	      implements_check = false;
	      //Didn't find a definition for the desired
	      //interface. --->Error
	      ReportError::IdentifierNotDeclared
		(implements_id, LookingForInterface);	    
	    }
	  else
	    {
	      //Okay, we found a declaration, make sure its
	      //an InterfaceDecl
	      InterfaceDecl* interface_decl = 
		dynamic_cast<InterfaceDecl*>(d);
	      
	      if(!interface_decl)
		{
		  ReportError::IdentifierNotDeclared
		    (implements_id, LookingForInterface);
		  implements_check = false;
		}
	      else
		{
		  //Found the interface, it looks valid.  Add
		  //reference to the list of interfaces to
		  //implememnt. 
		  interfaces_to_implement->append(interface_decl);
		}
	    }
	}   
      
    }

  //Jump into new scope for class
  Scope *savedScope = c->getScope();
  
  //Save the previous scope, and allow additions to the scope
  c->setPreviousScope(savedScope);
  c->newScope();
  c->resetNameCheck();
  c->setIdentifier();
  
  //Check all the variables declared in this scope
  members_check =  members->checkVarDecls(c);

  /* If cd is not NULL, we have been extended from 
   * a valid base class. This means we need to 
   * copy the scope from the base class into 
   * the current scope. */

  //copy stuff from parent class into scope 
  if(cd)
    {
      c->mergeOtherScope(cd->getScope());
    }
  
  
  if(cd) 
    {

      class_scope->mergeScopes(cd->getScope());
    }
 
  /* Merge scope with previous scope */
  c->mergeScope();

  //Disallow new additions to the scope
  c->clearIdentifier();

  //Get forward signatures for methods
  members->getFnSignatures(c);
  
  //pp4
  myScope = c->getScope();

  //Check semantic validity of the methods
  members_check = members->checkFnDecls(c)
    && members_check;
  
  //jump out of new class body scope  
  c->popScope(savedScope);
  
  //check self and any base classes
  extend_class(this,c);

  /* After checking methods for semantic validity, make sure
   * that've actually implemented all the methods for the interfaces
   * that we say we're implementing.  */
  if(implements)
    {
      for(int i = 0; i < interfaces_to_implement->size(); i++)
	{
	  //Pop an interface
	  InterfaceDecl* interface_decl = interfaces_to_implement->get(i);
	  assert(interface_decl);
	  
	  //Pass the interface all the methods we implement.
	  if(!interface_decl->checkMethods(methods,c))
	    {
	      //If the interface doesn't see a method it needs.....
	      ReportError::InterfaceNotImplemented(this, implements->get(i));
	    }
	}
      //add the implemented functions to the class
      for(int i = 0; i<implements->size(); i++)
	{
	  InterfaceDecl* id = dynamic_cast<InterfaceDecl*>(c->getDecl(implements->get(i)->getType()));
	  implement_class(id);
	}   
    }
  
  c->setClassScope(static_cast<ClassDecl*>(NULL));
  
  if(implements_check && extends_check && implements_check &&
     members_check)
    {
      return true;
    }
  else
    return false;
}

bool InterfaceDecl::checkMethods(List<FnDecl*> *fd, Check* c)
{
  bool found_signatures = true;
  
  bool *foundArr;

  
  foundArr = new bool[members->size()];

  for(int i = 0; i < members->size(); i++)
    {
      foundArr[i] = false;
    }

  for(int i =0; i < members->size(); i++)
    {
      FnDecl *d1 = static_cast<FnDecl*>(NULL);
 
      if(members->get(i)->isFnDecl()){
	d1 = dynamic_cast<FnDecl*>(members->get(i));
	
	for(int j = 0; j < fd->size(); j++)
	  {
	    FnDecl *d0 = static_cast<FnDecl*>(NULL);
	    
	    d0 = (fd->get(j));
	  
	    //names match?
	    if( (strcmp(d0->getName(),d1->getName())==0 )) 
	      {
		
		global_c = c;
		if(*d0 == *d1)
		  {
		    foundArr[i] = true;
		  }
		else
		  {
		    ReportError::OverrideMismatch(d0);
		    foundArr[i] = true;
		  }
	      }
	  }
      } else {
	foundArr[i] = true;
      }
    }
  for(int i = 0; i < members->size(); i++)
    {
      if(foundArr[i] == false)
	{
	  found_signatures = false;
	  break;
	}
    }

  delete [] foundArr;

  return found_signatures;
}

InterfaceDecl::InterfaceDecl(Identifier *n, List<Decl*> *m) : Decl(n) {
  assert(n != NULL && m != NULL);
  (members=m)->setParentAll(this);
  interface_decls = new List<FnDecl*>;
}

void InterfaceDecl::printChildren(int indentLevel) {
  id->print(indentLevel+1);
  members->printAll(indentLevel+1);
}

List<Decl*> * InterfaceDecl::getMembers(){
  return members;
}

void InterfaceDecl::addPrototype(FnDecl *fd)
{
  interface_decls->append(fd);
}

/* Semantically check an interface 
 * declaration */
bool InterfaceDecl::checkChildren(Check *c)
{
  bool id_check = true;
  bool members_check = true;
  
  id_check = id->check(c);
  members_check = members->checkAll(c);

  //c->addDecl(this);

  if(id_check && members_check)
    return true;
  else 
    return false;

}

/* Search an interface declarationf for 
 * a specific method name */
FnDecl* InterfaceDecl::findMethod(char *name){

  for(int i = 0; i<interface_decls->size();i++){
    if(strcmp(interface_decls->get(i)->getName(),name)==0){
      return interface_decls->get(i);
    }
  }
  return NULL;
}


FnDecl::FnDecl(Identifier *n, Type *r, List<VarDecl*> *d) : Decl(n) {
  assert(n != NULL && r!= NULL && d != NULL);
  (returnType=r)->setParent(this);
  (formals=d)->setParentAll(this);
  body = NULL;
}


llvm::FunctionType *FnDecl::getFuncSignature(llvm::LLVMContext *Context,
					     std::set<ClassDecl*> &classes,
					     std::vector<std::string> &argNames)
{
  llvm::FunctionType *blockFunctionType = 0;
  std::vector<llvm::Type*> blockArgTypes;
  ClassDecl *myClass = dynamic_cast<ClassDecl*>(getParent());
  bool isMethod = (myClass != 0);

  /* if method, first argument is the "this" parameter */
  if(isMethod) {
    classes.insert(myClass);
    
    std::string className = std::string("_this_") + 
      std::string(myClass->getName());
    
    argNames.push_back(className);
    blockArgTypes.push_back(llvm::Type::getInt32Ty(*Context)); 
  }
  
  for(int i = 0; i<formals->size(); i++)
    {
      VarDecl *vD = formals->get(i);
      argNames.push_back(std::string(vD->getName()));
      blockArgTypes.push_back(llvm::Type::getInt32Ty(*Context));
    }
  



  llvm::ArrayRef<llvm::Type*> blockArgs(blockArgTypes);
  blockFunctionType= 
    llvm::FunctionType::get(llvm::Type::getInt32Ty(*Context),
			    blockArgs,
			    false);
  //assert(returnType == Type::voidType);
  //printf("return type %s\n", returnType->getType()); 
  //blockFunctionType->dump();
  return blockFunctionType;
}

void FnDecl::emit()
{
  Scope * tempScope = emitScope;
  BeginFunc* funcStart = 0; 
  ClassDecl *myClass = 0;

  myClass = dynamic_cast<ClassDecl*>(getParent());
  emitScope = this->myScope;

  //check if the function is actually a method
  if(myClass)
    {
      char* strMethod = id->getName();
      char* strClass = myClass->getName();

      int strMethodLen = strlen(strMethod);
      int strClassLen = strlen(strClass);
      
      int memBytes = (1 + strClassLen + 1 + strMethodLen + 1);

      //Allocate enough memory for complete label
      //namespace labeling = _ (class name) . (method name)
      
      CodeLabel = new char[memBytes]; 
      
      //generate label
      snprintf(CodeLabel,memBytes, "_%s.%s", strClass, strMethod);

      cg->GenLabel(CodeLabel);
      funcStart = cg->GenBeginFunc();   
      funcStart->func = this;

      //Add "this" parameter
      char* myString = myClass->getName();
      int munged_bytes = strlen(myString) + 16;
      char* munged_this = new char[munged_bytes];
  
      snprintf(munged_this, munged_bytes, "_this_%s", myString);
  
      Location *this_location = cg->GenParamVar(munged_this, 0);
      cg->setThis(this_location);

      //associate the arguments passed in with the vardecls
      for(int i = 0; i<formals->size(); i++)
	{
	  formals->get(i)->SetLocation(cg->GenParamVar( formals->get(i)->getName(),(i + 1)));
	  if(formals->get(i)->getVarType() == Type::doubleType)
	    {
	      assert(enable_float);
	      formals->get(i)->GetLocation()->SetFloat(true);
	    }
	}
      
      Hashtable<int> *vars = myClass->getClassVarOffsets();
      Iterator<int> iter = vars->iterator();
      const char * str;
      int this_var_offset =  iter.value();
      while (( str= iter.nextkey()) != NULL) {
	if(strcmp(str,"*vptr")!=0)
	  {
	    char * fakename = new char[32+strlen(str)+1];
	    sprintf(fakename,"*_this_%s",str);
	    Decl *temp_decl = nullptr;
	    temp_decl = emitScope->find(fakename);
	    if(nullptr == temp_decl) {
	      size_t cpyLen = strlen(str);
	      char *realname = new char[cpyLen + 1];
	      strcpy(realname,str);
	      temp_decl = (emitScope->find(realname));
	    }

	  Location *this_var_offset_location 
	    = cg->GenLoadConstant(this_var_offset);
	  Location *this_var 
	    = cg->GenBinaryOp("+",this_location,this_var_offset_location);
	  temp_decl->SetLocation(this_var);
	} 
	this_var_offset = iter.value();
      }
    }
  //function
  else
    {
      //generate function label
      
      char* codeLabel = new char[1 + strlen(id->getName())+1]; 
      if(strcmp(id->getName(),"main")==0){
	sprintf(codeLabel, "%s",id->getName());
      } else {
	sprintf(codeLabel, "_%s",id->getName());
      }
      cg->GenLabel(codeLabel);

      funcStart = cg->GenBeginFunc();
      funcStart->func = this;

      for(int i = 0; i<formals->size(); i++)
	{
	formals->get(i)->SetLocation(cg->GenParamVar( formals->get(i)->getName(),i));

	if(formals->get(i)->getVarType() == Type::doubleType)
	  {
	    assert(enable_float);
	    formals->get(i)->GetLocation()->SetFloat(true);
	  }
      }
    }


  //enter body of function/method
  if(body)
    body->emit();

  cg->GenEndFunc();

  funcStart->SetFrameSize(4*cg->CodeSize());

  emitScope = tempScope;
}


void FnDecl::setFunctionBody(Stmt *b) {
  (body=b)->setParent(this);
}

void FnDecl::printChildren(int indentLevel) {
  returnType->print(indentLevel+1, "(return type) ");
  id->print(indentLevel+1);
  formals->printAll(indentLevel+1, "(formals) ");
  if (body) body->print(indentLevel+1, "(body) ");
}

/* Semantically check function declarations */
bool FnDecl::checkChildren(Check *c) 
{
  bool returnType_check = true;
  bool id_check = true;
  bool formals_check = true;
  bool body_check = true;

  returnType_check = returnType->check(c);
  id_check = id->check(c);
 
  /* Create a new scope and copy stuff
   * from the previous scope into it */
  Scope *savedScope = c->getScope();
  c->setPreviousScope(savedScope);
  c->setIdentifier();
  c->newScope();
  c->resetNameCheck();
  c->mergeScope();
  
  /* This is needed to know if we're
   * part of a function when we handle
   * scope in StmtBody->checkChildren() */
  c->setFnScope(this);

  //Check all the formals
  formals_check = formals->checkAll(c);
  myScope = c->getScope();
  /* Check the function body */
  if (body) 
    body_check = body->check(c);
   
  //Jump out of the previous scope
  c->popScope(savedScope);
  
  ClassDecl* myClass = dynamic_cast<ClassDecl*>(parent);
  InterfaceDecl* myDecl = dynamic_cast<InterfaceDecl*>(parent);  

  /* If the method is part of an interface, add "this" reference
   * to the declaration list */

  if(myDecl)
    myDecl->addPrototype(this);
    
  //Jump out of the function scope
  c->setFnScope(static_cast<FnDecl*>(NULL));
  
  if(returnType_check && id_check && formals_check &&
     body_check)
    return true;
  else 
    return false;
}


/* Compare two function declarations for identical 
 * name, return and parameter types */ 
bool FnDecl::operator==(const FnDecl &rhs) const
{
  bool name_check = (*(this->id) == *(rhs.id));
  bool return_check = strcmp(this->returnType->getType(), rhs.returnType->getType())==0;

  bool num_formals_check = 
    (this->formals->size() == rhs.formals->size());

  bool formals_check = true;

  //make sure arguments match in number and type
  if(num_formals_check)
    {
      for(int i = 0; i < this->formals->size(); i++)
	{
	  VarDecl *vd0 = this->formals->get(i);
	  VarDecl *vd1 = rhs.formals->get(i);
	  if(!Type::CompareTypes(global_c, vd0->getVarType(), vd1->getVarType())){
	    formals_check = false;
	    break;  
	  }
	}
    }
  return (name_check && return_check && 
	  num_formals_check && formals_check);

}

bool FnDecl::operator!=(const FnDecl &rhs) const
{
  return !(*this == rhs);
  
}

/* This method makes sure a method (haha) is being passed valid
 * parameters (number, type, etc). */
bool FnDecl::validParameters(List<Expr*> *p, Identifier* where, Check* c)
{
  bool match = true;
  int formals_count = formals->size();
  int parameters_count = p->size();

  assert(where);
  match = (formals_count == parameters_count);

  if(!match)
    {
      //Number of parameters does not match
      ReportError::NumArgsMismatch(where, formals_count, parameters_count); 
    }

  else
    {
      //Iterate over all arguments
      for(int i = 0; i < formals_count; i++)
	{
	  Expr *e = p->get(i);
	  Type *t0 = e->ExprType;
	  Type *t1 = formals->get(i)->getVarType();
	  match = Type::CompareTypes(c,t1, t0);
	  if(!match) 
	    {
	      ReportError::ArgMismatch(e,(i+1),t0,t1);
	      break;
	    }
	}
      
    }
  

  return match;

}

