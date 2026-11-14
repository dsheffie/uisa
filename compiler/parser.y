/* File: parser.y
 *
 * Yacc input file to generate the parser for the compiler.
 *
 * pp2: your job is to write a parser that will construct the parse tree and if
 *      no parse errors were found, print it.  The parser should accept the
 *      language as described in Decaf specification, and as augmented in the
 *      pp2 handout.
 */

%{

/* Just like lex, the text within this first region delimited by %{ and %} is
 * assumed to be C/C++ code and will be copied verbatim to the y.tab.c file
 * ahead of the definitions of the yyparse() function. Add other header file
 * inclusions or C++ variable declarations/prototypes that are needed by your
 * code here.
 */
#include "scanner.h" // for yylex
#include "parser.h"
#include "errors.h"
#include "debug.h"

/* Operators (+,-,etc) tokens for Operator class */
#include "char_tokens.h"

/* Standard error-handling routine */
void yyerror(char *msg);

/* Populated when the parse is done */
Program* program_result = NULL; 



%}

/* The section before the first %% is the Definitions section of the yacc input
 * file. Here is where you declare tokens and types, add precedence and
 * associativity options, and so on.
 */

/* Here we define the type of the yylval global variable that is used by the
 * scanner to store attibute information about the token just scanned and thus
 * communicate that information to the parser.
 *
 * pp2: You will need to add new fields to this union as you add different
 *      attributes to your non-terminal symbols.
 */
%union {
  int    intConst;
  bool   boolConst;
  char   *strConst;
  double dblConst;
  char   identifier[MaxIdentLen+1]; /* +1 for terminating null */
  
  Decl *decl;
  List<Decl*> *declList;
  Identifier *Ident;
  Type  *varType;
  
  /* Declarations */
  VarDecl *variableDecl;
  
  InterfaceDecl *interfaceDecl;
  FnDecl *fnDecl;
  
  StmtBlock *stmtBlock;
  
  
  List<VarDecl*> *varList;
  
  /* Stuff for classes */
  /* Extends */
  NamedType* extendsType;
  
  /* Implmenents list */
  List<NamedType*> *impList;
  ClassDecl *classDecl; 
  
  
  Stmt *stmt;
  
  List<Stmt*> *stmtList;
  
  
  /* For If and If else */
  IfStmt *ifStmt;
  ForStmt *forStmt;
  WhileStmt *whileStmt;
  BreakStmt *breakStmt;
  PrintStmt *printStmt;
  ReturnStmt* returnStmt;
   

  //For switch statements
  SwitchStmt* switchStmt;
  List<CaseStmt*> *caseStmts;
  CaseStmt *caseStmt;   

  List<Expr*> *exprList;
  
  Expr   *expr;
  LValue *lval;
}


/* Here we tell yacc about all the token types that we are using.  Yacc will
 * assign unique numbers to these and export the #define in the generated
 * y.tab.h header file.
 */
%token   T_Void T_Bool T_Int T_Double T_String T_Class
%token   T_LessEqual T_GreaterEqual T_Equal T_NotEqual T_Dims
%token   T_And T_Or T_Null T_Extends T_This T_Interface T_Implements
%token   T_While T_For T_If T_Else T_Return T_Break
%token   T_Print T_ReadInteger T_ReadLine T_New T_NewArray T_Malloc
%token   T_GetByte T_SetByte
%token   T_Switch T_Default T_Case T_PlusPlus T_MinusMinus

 /*Basic non-terminals types*/
%token   <intConst>      T_IntConst
%token   <boolConst>     T_BoolConst
%token   <strConst>      T_StrConst
%token   <dblConst>      T_DblConst
%token   <identifier>    T_Identifier


/* Non-terminal types: In order for yacc to assign/access the correct field of
 * $$, $1, etc., we must to declare which field is appropriate for the
 * non-terminal.  As an example, this first type declaration establishes that
 * the DeclList non-terminal uses the field named "declList" in the yylval
 * union. This means that when we are setting $$ for a reduction for DeclList
 * ore reading $n which corresponds to a DeclList nonterminal we are accessing
 * the field of the union named "declList" which is of type List<Decl*>.
 *
 */
 
 /*Declaration terminals types*/
%type   <declList>       DeclList
%type   <decl>           Decl
%type   <Ident>	 	 Ident
%type   <varType>        Type
%type   <variableDecl>   Variable
%type   <varList>        VariableDecls
%type   <variableDecl>   VariableDecl
%type   <fnDecl>         FunctionDecl
%type   <fnDecl>         Prototype 
%type   <declList>       Prototypes
%type   <decl>           InterfaceDecl
%type   <decl>           ClassDecl
%type   <varList>        Formals
%type   <varList>        VariableList

 /*Expr nonterminal types*/
%type   <expr>           Constant
%type   <expr>           Expr
%type   <expr>           OptionalExpr 
%type   <expr>           NotOptionalExpr
%type   <expr>           Call
%type   <lval>           LValue

 /* Class nonterminial types */
%type   <impList>        OptionalImplements
%type   <extendsType>    OptionalExtends
%type   <decl>           Field
%type   <declList>       Fields


 /* Statement nonterminal types */
%type  <ifStmt>          IfStmt	
%type  <forStmt>         ForStmt
%type  <whileStmt>       WhileStmt
%type  <breakStmt>       BreakStmt
%type  <printStmt>       PrintStmt
%type  <returnStmt>      ReturnStmt
%type  <stmt>            Stmt
%type  <stmtList>        Stmts
%type  <stmtBlock>       StmtBlock
%type  <stmtBlock>       VStuff
%type  <expr>		 Assign

/*Switch nonterminal types*/
%type  <switchStmt>      SwitchStmt
%type  <caseStmts>	 SwitchInside
%type  <caseStmt>  	 CaseInside
%type  <expr>		 CaseIntConst
%type  <stmtList>        DefaultInside

 /*Actual nonterminal type*/
%type  <exprList>        ExprList
%type  <exprList>        Actuals	 
%type  <exprList>        SingleActuals
%type  <exprList>        DoubleActuals

/* Associativty and Precedence (low to high)  */

/* Precedence to resolve else statements*/
%nonassoc LOWER_THAN_ELSE
%nonassoc T_Else

 /* Operator Precedence: 
  * Used to resolve shift/reduce errors
  *
  * These were pretty much copied straight from 
  * the Decaf specification */

%nonassoc '='
%left T_Or
%left T_And
%nonassoc T_Equal T_NotEqual
%nonassoc '<' T_LessEqual '>' T_GreaterEqual
%left '+' '-'
%left '*' '/' '%' 
%left '!'
%left UMINUS T_PlusPlus T_MinusMinus
%left '[' '.'


%%
/* Rules: All productions and actions should be placed between the start and
 * stop %% markers which delimit the Rules section.
 */

 /* Production to generate the whole program */
Program : DeclList                 
	{
 	  program_result = new Program($1); 
	};

/* Production to generate lists of declarations */
DeclList  : DeclList Decl          
           { 
             ($$=$1)->append($2); 
           }
          | Decl                   
           { 
             ($$ = new List<Decl*>)->append($1); 
	   };

/* Production to choose declaration type */
Decl      : VariableDecl	{ $$ = $1; }
          | FunctionDecl	{ $$ = $1; }
          | ClassDecl		{ $$ = $1; }
          | InterfaceDecl	{ $$ = $1; }
	  ;


/* Production to generate a complele variable declaration */ 
VariableDecl	: Variable ';'	{ $$ = $1; };


/* Production to generate a variable declaration */ 
Variable	: Type Ident	
                 { 
                   $$ = new VarDecl($2, $1); 
                 };

/* Production to generate parse type */ 
Type		: T_Int		{ $$ = Type::intType; }
                | T_Double	{ $$ = Type::doubleType; }
                | T_Bool	{ $$ = Type::boolType;  }
                | T_String	{ $$ = Type::stringType; }
                | Type T_Dims	{ $$ = new ArrayType(@1, $1); } 
                | Ident	        { $$ = new NamedType($1);  } 
          	;

/* Productions to recognize identifiers or the two
 * "special" LibCalls: SetByte and GetByte. 
 */
Ident     	: T_Identifier	{$$ = new Identifier(@1, $1);}
		| T_GetByte 
                {
		  /*Since GetByte is s special word from the lexer
		   *we ignore its special meaning in all context except 
		   *libcalls described later*/
		  $$ = new Identifier(@1, "GetByte");
		}
		| T_SetByte {
		  /*Since SetByte is s special word from the lexer
		   *we ignore its special meaning in all context except 
		   *libcalls described later*/
		  $$ = new Identifier(@1, "SetByte");
		  }
		;

/* Parse function declarations. */
FunctionDecl	: Type Ident '(' Formals ')' StmtBlock	
                  {
		    $$ = new FnDecl($2, $1, $4);
		    ($$)->setFunctionBody($6);
                  }
                 /* Production to parse void functions */
		| T_Void Ident '(' Formals ')' StmtBlock
	          {
                    $$ = new FnDecl($2, Type::voidType, $4);
		    ($$)->setFunctionBody($6);
                  }
                  ;

/* Parse formals, aka function parameters.  We use right-recursion 
 * to build up the formals list.  The use of right-recursion requires
 * that we add nodes to the syntax tree with the prepend() method
 * in order to push nodes to the front of the list. 
*/
Formals		: Variable VariableList	
                  {
		    $$ = $2;
                    ($$)->prepend($1);
                  }
                /* Either the end of the parameter list, or 
		 * no parameters in the Formals.  Handle both
		 * cases by returning a new variable declaration
		 * list */
                | {
                    $$ =  new List<VarDecl*>;
                  }
                  ;

/* Construct a list of variables. Right-recursive, needs 
 * to use the prepend function  */
VariableList 	: ',' Variable VariableList	
                { 
		  $$ = $3;
                  ($$)->prepend($2);
                } 
                /* Either the end of the variable list, 
                 * or an empty variable list */
                | { 
                    $$ = new List<VarDecl*>;
                  };


/* Production to match a class declaration */
ClassDecl	: T_Class Ident OptionalExtends OptionalImplements 
                 '{' Fields '}'	
                  {
		   
                    $$ = new ClassDecl($2, $3, $4, $6);
                  }
	     ;

/* Parse extend production */
OptionalExtends	: T_Extends Ident   
                 {
		   $$ = new NamedType($2);
                 }
                 | { $$ = NULL; }
		;


/* Parse implements production.  List creation is right-recursive */
OptionalImplements	: T_Implements Ident OptionalImplements
                          {
			    $$ = $3;
			    //append should probably be prepend
                           ($$)->prepend(new NamedType($2));
                          }
                         | ',' Ident OptionalImplements
                          {
			    //append should probably be prepend
                            ($$=$3)->prepend(new NamedType($2));
                          }
                         | 
                         {
                            $$ = new List<NamedType*>;
                         } ;

/* Production to build field list. Right-recursive, uses
 * prepend list method.  */
Fields	     : Field Fields	
               {
		 $$ = $2;
		($$)->prepend($1);
               } 
             | 
             { 
               $$ = new List<Decl*>;
             };

/* Field elements */
Field 	     : VariableDecl { $$ = $1; }
             | FunctionDecl { $$ = $1; }
	     ;

/* Production to build an interface declaration */
InterfaceDecl: T_Interface Ident '{' Prototypes '}'
               {
		 $$ = new InterfaceDecl($2, $4);
               }
	       ;

/* Production to build list of prototypes.  
 * Right recursive */
Prototypes     : Prototype Prototypes
                 {
		   $$ = $2;
		   ($$)->prepend($1);
                 }
	       | 
                 {
		   $$ = new List<Decl*>;
                 }
                 ;

/* Prototype elements */
Prototype      : Type Ident '(' Formals ')' ';'	
                  {
		    $$ = new FnDecl($2, $1, $4);
                  }
               /* Handle void prototypes too */
	       | T_Void Ident '(' Formals ')' ';'
                  {
		    $$ = new FnDecl($2, Type::voidType, $4);
                  }
	       ;


/* Parse statement blocks. The VStuff non-terminal contains
 * lists of variable declarations and function declarations
 */
StmtBlock      : '{' VStuff '}'	{ $$ = $2; }
	       ;

/* Parse all the variable declarations,  then
 * parse the statements.  This expression of the 
 * decaf grammar eliminates the shift/reduce 
 * errors present if naively implementing the 
 * specification. */
VStuff 	       : VariableDecls Stmts	
                 {
		   $$ = new StmtBlock($1, $2);
		 }
	       ;

/* Construct variable declaration lists */
VariableDecls  : VariableDecls VariableDecl	
                 {
		   $$ = $1;
                   ($$)->append($2);
                 }
               |  { $$ = new List<VarDecl*>; }
	       ;

/* Construction statement lists */
Stmts	       :  Stmt  Stmts 
                  {
		    $$ = $2;
                    /* Right-recursive productions use 
                       prepend to build up parse list in 
                       correct order 
		    */
                    ($$)->prepend($1);
                  }
                /* Build new statement list for either an empty 
		 * statement list, or the end of statement list 
		 */
               |  { $$ = new List<Stmt*>; }
	       ;

/* Production to build statement non-terminal */
Stmt	       : OptionalExpr ';' { $$ = $1; }
	       | IfStmt	          { $$ = $1; }
	       | WhileStmt        { $$ = $1; }
	       | ForStmt          { $$ = $1; }
	       | BreakStmt        { $$ = $1; }
	       | ReturnStmt       { $$ = $1; }
	       | PrintStmt        { $$ = $1; }
	       | StmtBlock        { $$ = $1; }  
	       | SwitchStmt	  { $$ = $1; }
	       ;

/* "Optional Expressions" are either empty, or they are
 * compressed of Expr non-terminal or Assign non-terminal. 
 *
 * This expression preserves the synatic value of the =
 */
OptionalExpr   : NotOptionalExpr{$$= $1;}
               | 	{ $$ = new EmptyExpr(); }
	       ;

/* If an OptionalExpr is not comprsied of the empty string,
 * it must contain one of the two non-terminals.
 *
 * See commments above (for OptionalExpr) for more details.
 */
NotOptionalExpr : Expr	{ $$ = $1; }
               | Assign { $$ = $1; };


/* The If statement.  The dangling else problem was solved by
 * setting the second production (no else) to lower precidence
 * than the if / else pair */
IfStmt	       : T_If '(' NotOptionalExpr ')' Stmt T_Else Stmt
          	{
                  $$ = new IfStmt($3, $5, $7);
                }
               | T_If '(' NotOptionalExpr ')' Stmt  %prec LOWER_THAN_ELSE 
                {
                  $$ = new IfStmt($3, $5, NULL);
                }
               ;

/* Straightforward translation of the While production */
WhileStmt      : T_While '(' NotOptionalExpr ')' Stmt	
                 {
                   $$ = new WhileStmt($3, $5);
                 }
	       ;

/* Straightforward translation of the For production */
ForStmt	       : T_For '(' OptionalExpr ';' NotOptionalExpr ';' OptionalExpr ')' Stmt
                {
                  $$ = new ForStmt($3, $5, $7, $9);
                }
	       ;

/* Straightforward translation of the Return production */
ReturnStmt     : T_Return OptionalExpr ';'	
                 {
		   $$ = new ReturnStmt(@2, $2);
                 }
	       ;

/* Straightforward translation of the Break production */
BreakStmt      : T_Break ';'	
                 {
		   $$ = new BreakStmt(@1);
                 }
	       ;

/* Straightforward translation of the Print production */
PrintStmt      : T_Print '(' ExprList NotOptionalExpr ')' ';'	
                 {
		   $3->append($4);
                   $$ = new PrintStmt($3);
                 }
	       ;
/* The production for a switch statement.  The swtich statement is
 * comprised of an expression, a list of cases, 
 * and a default statement */
SwitchStmt     : T_Switch '(' NotOptionalExpr ')' '{' SwitchInside T_Case
CaseInside DefaultInside '}'
	       {
		 $6->append($8);
	         $$ = new SwitchStmt($3, $6, $9);
	       }
	       ;

/* The "meat" of a switch statement: parsing case statments */
SwitchInside   :SwitchInside T_Case CaseInside	
	       {	         
	         ($$=$1)->append($3);
	       }
	       |
	       {
	          $$ = new List<CaseStmt*>;
	       }
	       ; 

/* Parse the case statement and the following code block (a statement) */
CaseInside     :CaseIntConst ':' Stmts 
	       {
	         $$ = new CaseStmt($1, $3);
	       }
	       ;
DefaultInside : T_Default ':' Stmts {$$ = $3;}
                | {$$ = NULL;}
               ;


/* Recursively (left-recursion) build up a list
 * of expressions */
ExprList       : ExprList  NotOptionalExpr ','  
                 {
		   $$ = $1;
		   ($$)->append($2);
                 }
               | 
                 {
                   $$ = new List<Expr*>;
                 }
	       ;

/* The assign statement.  We have removed this production
 * from the non-terminal "Expr" to solve issues we had
 * parsing code like:
 * a + b = c;
 *
 * Without seperating the assignment statement. Bison
 * would perform the following reductions. 
 *
 * indent : (lookahead +)
 * Lvalue : (lookhahed +)
 * Expr   : (lookahead +)
 * Expr + indent (lookahead = )
 * Expr + Lvalue (lookahead = )
 * Expr + Lvalue  = (lookahead ) Lvalue.
 * Reduce Lvalue = Lvalue to Expr
 * Expr + Expr 
 * Reduce Expr + Expr to Expr
 *
 * This is obviously not what we want to happen.  
 * Removing the Assign production from Expr prevents 
 * this from occuring.  
 *
 * That said, a couple of tweaks must be made to the 
 * Expr expression to keep the same syntatic meeting.
 *
 * In particular, we had to add a Expr -> ( NotOptionalExpr )
 * production 
 */

Assign :  LValue '=' Expr
                {
		  Operator *o = new Operator(@2, tokEq);
		  $$ = new AssignExpr($1, o , $3); 
                };
    
/* Except for the commments mentioned for the "Assign" production,
 * the Expr production is a straighforward translation of the 
 * grammar presented in the Decaf spec */
Expr	 :     Constant   	{ $$ = $1; }
               | LValue		{ $$ = $1; }
               | T_This		{ $$ = new This(@1); }
               | Call		{ $$ = $1; }
               | '(' NotOptionalExpr ')'	{ $$ = $2; }
               | Expr '+' Expr 	
                 {  
		   Operator *o = new Operator(@2, tokAdd);
		   $$ = new ArithmeticExpr($1, o, $3); 
		 }

	       | Expr '-' Expr	
                 {
		   Operator *o = new Operator(@2, tokSub);
		   $$ = new ArithmeticExpr($1, o, $3); 
		 }

	       | Expr '*' Expr	
                 {
		   Operator *o = new Operator(@2, tokMul);
		   $$ = new ArithmeticExpr($1, o, $3); 
		 }

	       | Expr '/' Expr	
                 {
                   Operator *o = new Operator(@2, tokDiv);
		   $$ = new ArithmeticExpr($1, o, $3); 
                                 }

	       | Expr '%' Expr	
                 { 
		   Operator *o = new Operator(@2, tokMod);
		   $$ = new ArithmeticExpr($1, o, $3); 
                 }

	       | '-' Expr %prec UMINUS	
                 {
		   Operator *o = new Operator(@1, tokSub);
		   $$ = new ArithmeticExpr(o, $2); 
		 }

	       | Expr '<' Expr	
                 { /*tokLess */
		   Operator *o = new Operator(@2, tokLess);
		   $$ = new RelationalExpr($1, o, $3); 
                 }


	       | Expr '>' Expr
                 { /*tokGreater */
		   Operator *o = new Operator(@2, tokGreater);
		   $$ = new RelationalExpr($1, o, $3); 
                 }



	       | Expr T_LessEqual Expr	
                 { /*tokLessEq */
		   Operator *o = new Operator(@1, tokLessEq);
		   $$ = new RelationalExpr($1, o, $3); 
                 }



	       | Expr T_GreaterEqual Expr
                 { /*tokGreaterEq */
		   Operator *o = new Operator(@2, tokGreaterEq);
		   $$ = new RelationalExpr($1, o, $3); 
                 }


	       | Expr T_Equal Expr	
                 { 
		   Operator *o = new Operator(@2, tokEquals);
		   $$ = new EqualityExpr($1, o, $3); 
                 }
	       | Expr T_NotEqual Expr	
                 { 
		   Operator *o = new Operator(@2, tokNotEquals);
		   $$ = new EqualityExpr($1, o, $3); 
                 }

	       | Expr T_And Expr
                 { 
		   Operator *o = new Operator(@2, tokAnd);
		   $$ = new LogicalExpr($1, o, $3); 
                 }

	       | Expr T_Or Expr		
                 { 
		   Operator *o = new Operator(@2, tokOr);
		   $$ = new LogicalExpr($1, o, $3); 
                  }

	       | '!' Expr  		
         	{
		   Operator *o = new Operator(@1, tokNot);
		   $$ = new LogicalExpr(o, $2); 
                }


	       | T_New '(' Ident ')'	
         	{
		  yyltype join = loc_join(@1,@4);
                  NamedType *n = new NamedType($3);
		  $$ = new NewExpr(join, n);
                }

	       | T_NewArray '(' NotOptionalExpr ',' Type ')'
         	{
		  yyltype join = loc_join(@1,@6);
		  $$ = new NewArrayExpr(join, $3, $5);
                }

               | T_ReadInteger '(' ')'	      		
                 {
		   yyltype join = loc_join(@1,@3);
		   $$ = new ReadIntegerExpr(join);
                 }

	       | T_ReadLine '(' ')'			
		 {
		   yyltype join = loc_join(@1,@3);
                   $$ = new ReadLineExpr(join);
                 }

	       | T_Malloc '(' NotOptionalExpr ')'	
		 {
		   yyltype join = loc_join(@1,@4);
                   $$ = new MallocExpr(join, $3);
                 }
               | LValue T_PlusPlus 			
	       {
		 /* ++ and -- are similar to unary minus, except the rhs
		  * expr is null, and we assume only LVALUE can have ++
		  * our assumption is  that ++, -- can only be performed
		  * on exprs with storage, same as the assignemnt operator
		  */
		 Operator *o = new Operator(@2, tokPlusPlus);
		 $$ = new ArithmeticExpr($1, o); 
	       }
	       | LValue T_MinusMinus			
	       {
		 Operator *o = new Operator(@2, tokMinusMinus);
		 $$ = new ArithmeticExpr($1, o);
	       }
	       ;

/* Straightforward translation of the LValue production */
LValue	       : Ident			
                 {  
		  $$ =  new FieldAccess(NULL, $1);
                 } 
	       | Expr '.' Ident	
                 {  
		   $$ =  new FieldAccess($1, $3);
                 } 
               | Expr '[' NotOptionalExpr ']'	
                 { 
		  yyltype join = loc_join(@1,@4);
                  $$ = new ArrayAccess(join, $1, $3); 
                 }
	       ;

/* Straightforward translation of the Call production */
Call	       : Ident '(' Actuals ')'		
                 {  
		  yyltype join = loc_join(@1,@4);
                  $$ = new Call(join, NULL, $1, $3); 
                 }
	       | Expr '.' T_Identifier '(' Actuals ')'
           	{
		  yyltype join = loc_join(@1,@6);
		  Identifier *temp = new Identifier(@3	, $3);
                  $$ = new Call(join, $1, temp, $5); 
                }
 	       |Expr '.' T_GetByte '(' SingleActuals ')'
	       {
		 /*Special library calls utilizing the GetByte and SeByte
		  *tokens
		  */
		 $$ = new GetByte(@3, $1, ($5)->get(0)); 
                }
               /* Print errors if user enters incorrect number of parameters */
	       | Expr '.' T_SetByte '(' DoubleActuals ')'	
	       	 { 
		   $$ = new SetByte(@3, $1, ($5)->get(0),($5)->get(1)); 
		 }
                ;


/* Straightforward translation of the Actuals production */
Actuals	       : ExprList NotOptionalExpr
                 {  
                   $$ = $1; 
		   $$->append($2); 
                 }
                |
                {
		  $$ = new List<Expr*>;
                }
	       ;
/* Build actuals for components */
SingleActuals  : NotOptionalExpr
	       { ($$=new List<Expr*>)->append($1);
	       }
	       ;
DoubleActuals  : NotOptionalExpr ',' NotOptionalExpr
	       { ($$=new List<Expr*>)->append($1);
	         $$->append($3);
	       }
	       ;

/* Terminals for constant types */
Constant       : T_IntConst	{ $$ = new IntConstant(@1, $1); }
               | T_DblConst     { $$ = new DoubleConstant(@1, $1); }
               | T_BoolConst    { $$ = new BoolConstant(@1, $1);}
               | T_Null		{ $$ = new NullConstant(@1); }
               | T_StrConst	{ $$ = new StringConstant(@1, $1); }

/* Constants for case statements (must be an integer) */
CaseIntConst  : T_IntConst	{ $$ = new IntConstant(@1,$1); }
	       ;

%%		

/* The closing %% above marks the end of the Rules section and the beginning of
 * the User Subroutines section. All text from here to the end of the file is
 * copied verbatim to the end of the generated y.tab.c file.  This section is
 * where you put definitions of helper functions.
 */

/**
 * This function will be called before any calls to yyparse().  It is designed
 * to give you an opportunity to do anything that must be done to initialize the
 * parser (set global variables, configure starting state, etc.). One thing it
 * already does for you is assign the value of the global variable yydebug that
 * controls whether yacc prints debugging information about parser actions
 * (shift/reduce) and contents of state stack during parser.  If set to false,
 * no information is printed. Setting it to true will give you a running trail
 * that might be helpful when debugging your parser.
 */
void parser_init()
{
    yydebug = false; /* must be false when submitting! */
}

Program* parser_get_result()
{
    return program_result;
}
