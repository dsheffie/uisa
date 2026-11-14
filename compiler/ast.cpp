/* File: ast.cpp */

#include <stdio.h>
#include <string.h>
#include "ast.h"
#include "ast_type.h"
#include "ast_decl.h"
#include "debug.h"
#include "errors.h"

//pp4
#include "codegen.h"
#include "tac.h"

Node::Node(yyltype loc) {
    location = new yyltype(loc);
    parent = NULL;
}

Node::Node() {
    location = NULL;
    parent = NULL;
}

//return node's code location
Location* Node::GetLocation()
{
  return myLocation;
}

//forcefully change the location of this node
void Node::SetLocation(Location* l){
  myLocation = l;
}

//add node to the stack before a function call
void Node::PushOntoStack()
{
  cg->GenPushParam(myLocation);
}

/* The print method is used to print the parse tree nodes.
 * If this node has a location (most nodes do, but some do not), it
 * will first print the line number to help you match the parse tree
 * back to the source text. It then indents the proper number of levels
 * and prints the "print name" of the node. It then will invoke the
 * virtual function printChildren which is expected to print the
 * internals of the node (itself & children) as appropriate.
 */
void Node::print(int indentLevel, const char *label) {
    const int numSpaces = 3;
    printf("\n");
    if (getLocation())
        printf("%*d", numSpaces, getLocation()->first_line);
    else
        printf("%*s", numSpaces, "");
    printf("%*s%s%s: ", indentLevel*numSpaces, "",
           label? label : "", getPrintNameForNode());
   printChildren(indentLevel);
}

//emit nothing by default
void Node::emit()
{

}

bool Node::check(Check *c)
{
  return checkChildren(c);
}


bool Identifier::operator==(const Identifier &rhs) const
{
  return(strcmp(this->name, rhs.name)==0);
}

bool Identifier::operator!=(const Identifier &rhs) const
{
  return !(*this == rhs);
}	

Identifier::Identifier(yyltype loc, const char *n) : Node(loc) {
    name = strdup(n);
}

Identifier::Identifier(const char *n) {
    name = strdup(n);
}

void Identifier::printChildren(int indentLevel) {
    printf("%s", name);
}

//find the identifier in the scope, and return its location
void Identifier::emit()
{
  
  Decl* me = emitScope->find(this->getName());
  this->myLocation = me->GetLocation();
}

/*check an identifier make sure it exists in the current scope*/
bool Identifier::checkChildren(Check *c)
{

  bool check_id = true;
  
  /*check scope*/
  check_id = c->isValidVarName(name);
  return check_id;
}

char* Identifier::getName()
{
  return strdup(name);
}
