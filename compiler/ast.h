// -*-c++-*-
/* File: ast.h
 *
 * This file defines the abstract base class Node and the concrete Identifier
 * and Error node subclasses that are used through the tree as leaf nodes. A
 * parse tree is a hierarchical collection of ast nodes (or, more correctly, of
 * instances of concrete subclassses such as VarDecl, ForStmt, and AssignExpr).
 *
 * Printing: The only interesting behavior of the node classes for pp2 is the
 * bility to print the tree using an in-order walk.  Each node class is
 * responsible for printing itself/children by overriding the virtual
 * PrintChildren() and getPrintNameForNode() methods. All the classes we provide
 * already implement these methods, so your job is to construct the nodes and
 * wire them up during parsing. Once that's done, printing is a snap!
 *
 * Semantic analysis: For pp3 you are adding "Check" behavior to the ast
 * node classes. Your semantic analyzer should do an inorder walk on the
 * parse tree, and when visiting each node, verify the particular
 * semantic rules that apply to that construct.
 */

#ifndef AST_H
#define AST_H

#include <stdlib.h>
#include "location.h"
#include <iostream>
#include "check.h"
#include "tac.h"

using namespace std;

class Node
{
  protected:
    /** lexical location of the node; NULL for nodes 
     *that don't care/use locations */
    yyltype *location;

    /** pointer to the parent node in the parse tree */
    Node *parent;
    Location *myLocation;


  public:
    Node(yyltype loc);
    Node();
    virtual ~Node() {}

    //for pp4
    virtual Location* GetLocation();
    virtual void SetLocation(Location * l);
    virtual void PushOntoStack();

    yyltype *getLocation()   { return location; }
    void setParent(Node *p)  { parent = p;      }
    Node *getParent()        { return parent;   }

    virtual const char *getPrintNameForNode() = 0;

    // print() is deliberately _not_ virtual
    // subclasses should override printChildren() instead
    void print(int indentLevel, const char *label = NULL);

    virtual void printChildren(int indentLevel) {}

    /*checker function for every node*/
    bool check(Check *c);

    virtual void emit();

    /*checker function for every node*/
    virtual bool checkChildren(Check *c) {
      return true;
    }
    /*other virtual functions for classes inheriting node*/
    virtual bool isVarDecl() { return false;}
    virtual bool isFnDecl() { return false; }
    virtual bool isClassDecl() { return false; }
    virtual bool isInterfaceDecl() { return false; }

};



class Identifier : public Node
{
  protected:
    char *name;

  public:
    bool operator==(const Identifier &rhs) const;
    bool operator!=(const Identifier &rhs) const;
    Identifier(yyltype loc, const char *name);
    Identifier(const char* name);
    const char *getPrintNameForNode() override {
      return "Identifier";
    }
    void printChildren(int indentLevel) override;

    /*check*/
    bool checkChildren(Check *c) override;
    char *getName();
    
    friend ostream& operator<<(ostream& out, Identifier *id)
    { return out << id->name; }

    void emit() override;
};


/**
 * This node class is represents a portion of the tree that encountered syntax
 * errors during parsing. The partial completed tree is discarded along with the
 * states being popped, and an instance of the Error class can stand in as the
 * placeholder in the parse tree when your parser can continue after an error.
 */
class Error : public Node
{
  public:
    Error() : Node() {}
    const char *getPrintNameForNode() override {
      return "Error";
    }
};

#endif /* AST_H */
