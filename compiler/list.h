/* File: list.h
 *
 * Simple list class for storing a linear collection of elements.  This class is
 * nothing more than a very thin cover of a STL deque, with some added
 * range-checking. Given not everyone is familiar with the C++ templates, this
 * class provides a more familiar interface.
 *
 * It can handle elements of any type, the typename for a List includes the
 * element type in angle brackets, e.g.  to store elements of type double, you
 * would use the type name List<double>, to store elements of type Decl *, it
 * woud be List<Decl*> and so on.
 *
 * Here is some sample code illustrating the usage of a List of integers
 *
 *   int Sum(List<int> *list)
 *   {
 *       int sum = 0;
 *       for (int i = 0; i < list->size(); i++) {
 *          int val = list[i];
 *          sum += val;
 *       }
 *       return sum;
 *    }
 */

#ifndef LIST_H
#define LIST_H

#include <deque>
#include "debug.h"
#include "check.h"

class Node;
class Check;

template<class Element> class List {

 private:
    std::deque<Element> elems;

    void checkIndex(int index, bool lastOk = false) {
        int max = this->size() + (lastOk ? 1 : 0);
    }

 public:
    // Create a new empty list
    List() {}

    // Returns count of elements currently in list
    int size() const { return elems.size(); }

    // Returns element at index in list. Indexing is 0-based.
    // Raises an assert if index is out of range.
    Element get(int index) const {
        if(index < 0 && index >= this->size()) {
          die("List Assertion Failed: index is out of bounds");
        }

        return elems[index];
    }

    // Sets element at index in list. Indexing is 0-based.
    // Raises an assert if index is out of range.
    void set(int index, const Element &value) {
        if(index < 0 && index >= this->size()) {
            die("List Assertion Failed: index is out of bounds");
        }
        elems[index] = value;
    }

    /** returns the last element in the list or raises an assert if the list is empty */
    Element last() {
        if( size() == 0 ) die("List Assertion Failed: cannot use last() on an empty list");
        return get( size() - 1 );
    }

    // Inserts element at index, shuffling over others
    // Raises assert if index out of range
    void insert(const Element &elem, int index) {
        if(index < 0 || index > this->size()) {
            die("List Assertion Failed: index is out of bounds");
        }

        elems.insert(elems.begin() + index, elem);
    }


    /* Used with left-recursive productions */
    void append(const Element &elem) {
        elems.push_back(elem);
    }

    /* Used with right-recursive productions */
    void prepend(const Element &elem) {
        elems.push_front(elem);
    }


    // Adds all elements in another list to this list
    void appendAll(List<Element> &list) {
        for (int i = 0; i < list.size(); i++)
            append( list.get(i) );
    }

    // Removes all elements in this list which are also in the specified list
    void intersect(List<Element> &list) {
        for (int i=size()-1; i>=0; i--) {
            for (int j=0; j<list.size(); j++) {
                if( get(i) == get(j) ) {
                    remove(i);
                    break;    //go to next i (this one is toast!)
                }
            }
        }
    }

    // Removes all elements from the List
    void clear() {
        elems.clear();
    }

    // Removes element at index, shuffling down others
    // Raises assert if index out of range
    void remove(int index) {
        if(index < 0 || index >= this->size()) {
            die("List Assertion Failed: index is out of bounds");
        }

        elems.erase(elems.begin() + index);
    }

    void swap(int i1, int i2) {
        if(i1 < 0 || i1 >= this->size()) {
            die("List Assertion Failed: i1 is out of bounds");
        }

        if(i2 < 0 || i2 >= this->size()) {
            die("List Assertion Failed: i2 is out of bounds");
        }

        if( i1 == i2 ) return;

        Element e1 = get(i1);
        Element e2 = get(i2);
        remove(i1);
        remove(i2);

        if( i1 < i2 ) {
            insert(e2, i1);
            insert(e1, i2);
        } else {
            insert(e1, i2);
            insert(e2, i1);
        }
    }

    Element operator[](int index) const { return get(index); }

    /* These are some specific methods useful for lists of ast nodes They will
     * only work on lists of elements that respond to the messages, but since
     * C++ only instantiates the template if you use it you can still have Lists
     * of ints, chars*, as long as you don't try to *All on that list.
     */
    void setParentAll(Node *p) {
        for (int i = 0; i < size(); i++)
            get(i)->setParent(p);
    }

    void printAll(int indentLevel, const char *label = NULL) {
        for (int i = 0; i < size(); i++)
            get(i)->print(indentLevel, label);
    }
    
    void generateAllVTables()
    {
      for(int i = 0; i < size(); i++)
	{
	  if(get(i)->isClassDecl())
	    {
	      get(i)->genVTable();
	    }
	  
	}
      
    }



    //call emit all from a list of decls
    //variable first
    void emitAll()
    {

      //gotta first emit the global var decls first
      for(int i = 0; i < size(); i++)
	{
	 
	  if(get(i)->isVarDecl()){
	    get(i)->emit();
	  }
	}
      for(int i = 0; i < size(); i++)
	{
	  if(!get(i)->isVarDecl())
	    {
	      get(i)->emit();
	    }
	}

    }
    
    //push the entire list on the stack
    void pushAll()
    {
      for(int i = 0; i < size(); i++)
	{
	  get(i)->PushOntoStack();
	}      
    }

    /*invoke the check function for each element in a list*/
    bool checkAll(Check *c) 
    {
      bool status = true;
      for (int i = 0; i < size(); i++)
	status = get(i)->check(c) ? status : false;

      return status;
    }

    /*invoke check function for all variable declarations*/
    bool checkVarDecls(Check *c) 
    {
      bool status = true;
      for (int i = 0; i < size(); i++)
	{
	  if(get(i)->isVarDecl())
	    {
	      status = get(i)->check(c) ? status : false;
	    }
	}
      return status;
    }

    /*invoke check function for all function declaration*/
    bool checkFnDecls(Check *c) 
    {
      bool status = true;
      for (int i = 0; i < size(); i++)
	{
	  if(get(i)->isFnDecl())
	    {
	      status = get(i)->check(c) ? status : false;
	    }
	}

      return status;
    }



    /*invoke check function for all class declaration*/
    bool checkClassDecls(Check *c) 
    {
      bool status = true;
      for (int i = 0; i < size(); i++)
	{
	  if(get(i)->isClassDecl())
	    status = get(i)->check(c) ? status : false;
	}

      return status;
    }

    /*invoke check function for all interface declaration*/
    bool checkInterfaceDecls(Check *c) 
    {
      bool status = true;
      for (int i = 0; i < size(); i++)
	{
	  if(get(i)->isInterfaceDecl())
	    {
	      status = get(i)->check(c) ? status : false;
	    }
	}

      return status;
    }

    /*invoke check function for all other declarations*/
   bool checkNotVarDecls(Check *c) 
    {
      bool status = true;
      for (int i = 0; i < size(); i++)
	{
	  if(!get(i)->isVarDecl())
	    status = get(i)->check(c) ? status : false;
	}

      return status;
    }

   void getInterfaceSignatures(Check *c)
   {
     for(int i = 0; i < size(); i++)
       {
	 if(get(i)->isInterfaceDecl())
	   {
	     get(i)->getSignature(c);
	   }

       }

   }

   
    void getFnSignatures(Check *c)
    {
      for (int i = 0; i < size(); i++)
	{
	  if(get(i)->isFnDecl())
	    {
	      get(i)->getSignature(c);
	    }
	}
    }
    
    void getClassSignatures(Check *c)
    {
      for (int i = 0; i < size(); i++)
	{
	  if(get(i)->isClassDecl())
	    {
	      get(i)->getSignature(c);
	    }
	}
    }
    
    void getVarSignatures(Check *c){
      for(int i = 0; i<size(); i++){
	if(get(i)->isVarDecl()){
	  get(i)->getSignature(c);
	}
      }
    }

    void getSignatures(Check *c){
     for (int i = 0; i < size(); i++)
	{
	  get(i)->getSignature(c);
	}
    }
    class iterator : public std::iterator<std::forward_iterator_tag, List> {
    protected:
      friend class List;
      typename std::deque<Element>::iterator  itr;      
    public:
      iterator() {}
    iterator(typename std::deque<Element>::iterator  itr) : itr(itr) {}
      bool operator==(const iterator &rhs) const {
	return itr == rhs.itr;
      }
      bool operator!=(const iterator &rhs) const {
	return itr != rhs.itr;
      }
      Element &operator*() {
	return *itr;
      }
      iterator operator++(int postfix) {
	iterator it = *this;
	itr++;
	return it;
      }
      iterator operator++() {
	itr++;
	return *this;
      }
    };
    iterator begin() {
      return iterator(elems.begin());
    }
    iterator end() {
      return iterator(elems.end());
    }
};

#endif /* LIST_H */
