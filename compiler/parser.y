/* parser.y: grammar of the millicode C subset (forked from Decaf's parser.y).
 *
 * The Java object model (classes, interfaces, arrays, strings, New, Print)
 * is gone; what is left is C's statements and expressions over the integer
 * types u8 u16 u32 u64 s8 s16 s32 s64 (int = s32, unsigned = u32), plain
 * structs, typedefs, inline functions with reference parameters.
 */

%{
#include <cstdio>
#include "ast.h"
#include "errors.h"

int yylex();
extern int yylineno;
void yyerror(const char *msg) { error(yylineno, "%s", msg); }

TransUnit *parse_result = nullptr;
std::map<std::string, Type> g_typeNames;
static StructDef *g_curStruct = nullptr;

static void declareTypeName(const char *n, const Type &t) {
  if (g_typeNames.count(n)) error(yylineno, "type '%s' defined twice", n);
  g_typeNames[n] = t;
}
%}

%union {
  Type *type;
  char *ident;
  int op;
  struct { uint64_t value; int bits; bool isUnsigned; } lit;
  Expr *expr;
  Stmt *stmt;
  Block *block;
  std::vector<Expr *> *exprs;
  std::vector<Param> *params;
  std::vector<DeclItem> *decls;
  FnDecl *fn;
  TransUnit *tu;
}

%token T_Void T_Const T_Entry T_Struct T_Typedef T_Inline T_While T_Do T_For T_If T_Else T_Switch T_Case T_Default
%token T_Return T_Break T_Continue T_Le T_Ge T_Eq T_Ne T_And T_Or T_Shl T_Shr T_Inc T_Dec
%token <type> T_Type
%token <op> T_OpAssign
%token <lit> T_IntConst
%token <ident> T_Identifier

%type <tu> TransUnit
%type <fn> FunctionDecl
%type <type> RetType TypeSpec
%type <params> Params ParamList
%type <block> Block Stmts
%type <stmt> Stmt DeclStmt ForInit
%type <decls> DeclItems
%type <expr> Expr OptExpr AssignExpr CondExpr OrExpr AndExpr BitOrExpr XorExpr BitAndExpr
%type <expr> EqExpr RelExpr ShiftExpr AddExpr MulExpr UnaryExpr PostfixExpr PrimaryExpr
%type <exprs> Args ArgList

%nonassoc LOWER_THAN_ELSE
%nonassoc T_Else

%%

Program     : TransUnit                  { parse_result = $1; }
            ;

TransUnit   : TransUnit FunctionDecl     { ($$ = $1)->fns.push_back($2); }
            | TransUnit T_Const TypeSpec DeclItems ';'
                                         { $$ = $1; for (auto &d : *$4) $$->consts.push_back(new ConstDecl(*$3, d)); }
            | TransUnit StructDecl       { $$ = $1; }
            | TransUnit T_Typedef TypeSpec T_Identifier ';'
                                         { $$ = $1; declareTypeName($4, *$3); }
            | /* empty */                { $$ = new TransUnit(); }
            ;

StructDecl  : T_Struct T_Identifier
                  { g_curStruct = new StructDef(); g_curStruct->name = $2;
                    Type t; t.sd = g_curStruct; declareTypeName($2, t); }
              '{' Fields '}' ';'
                  { if (g_curStruct->fields.empty()) error(yylineno, "struct '%s' has no fields", $2); }
            ;

Fields      : Fields TypeSpec FieldNames ';' { }
            | /* empty */                { }
            ;
FieldNames  : T_Identifier               { g_curStruct->fields.push_back({$1, *$<type>0}); }
            | FieldNames ',' T_Identifier { g_curStruct->fields.push_back({$3, *$<type>0}); }
            ;

TypeSpec    : T_Type                     { $$ = $1; }
            | T_Struct T_Type            { $$ = $2; if (!$2->isStruct()) error(yylineno, "'struct' before a type that is not a struct"); }
            ;

FunctionDecl: RetType T_Identifier '(' Params ')' Block
                                         { $$ = new FnDecl(*$1, $2, *$4, $6, false); }
            | T_Inline RetType T_Identifier '(' Params ')' Block
                                         { $$ = new FnDecl(*$2, $3, *$5, $7, false); $$->isInline = true; }
            | T_Entry RetType T_Identifier '(' Params ')' Block
                                         { $$ = new FnDecl(*$2, $3, *$5, $7, true); }
            ;

RetType     : TypeSpec                   { $$ = $1; }
            | T_Void                     { $$ = new Type(Type::voidType()); }
            ;

Params      : ParamList                  { $$ = $1; }
            | T_Void                     { $$ = new std::vector<Param>(); }
            | /* empty */                { $$ = new std::vector<Param>(); }
            ;

ParamList   : TypeSpec T_Identifier      { ($$ = new std::vector<Param>())->push_back(Param{*$1, $2, false}); }
            | TypeSpec '&' T_Identifier  { ($$ = new std::vector<Param>())->push_back(Param{*$1, $3, true}); }
            | ParamList ',' TypeSpec T_Identifier { ($$ = $1)->push_back(Param{*$3, $4, false}); }
            | ParamList ',' TypeSpec '&' T_Identifier { ($$ = $1)->push_back(Param{*$3, $5, true}); }
            ;

Block       : '{' Stmts '}'              { $$ = $2; }
            ;

Stmts       : Stmts Stmt                 { ($$ = $1)->stmts.push_back($2); }
            | /* empty */                { $$ = new Block(); }
            ;

DeclStmt    : TypeSpec DeclItems ';'     { $$ = new DeclStmt(*$1, *$2, false); }
            | T_Const TypeSpec DeclItems ';' { $$ = new DeclStmt(*$2, *$3, true); }
            ;

DeclItems   : T_Identifier               { ($$ = new std::vector<DeclItem>())->push_back(DeclItem{$1, nullptr, yylineno}); }
            | T_Identifier '=' AssignExpr { ($$ = new std::vector<DeclItem>())->push_back(DeclItem{$1, $3, yylineno}); }
            | DeclItems ',' T_Identifier { ($$ = $1)->push_back(DeclItem{$3, nullptr, yylineno}); }
            | DeclItems ',' T_Identifier '=' AssignExpr { ($$ = $1)->push_back(DeclItem{$3, $5, yylineno}); }
            ;

Stmt        : Block                      { $$ = $1; }
            | DeclStmt                   { $$ = $1; }
            | Expr ';'                   { $$ = new ExprStmt($1); }
            | ';'                        { $$ = new Block(); }
            | T_If '(' Expr ')' Stmt %prec LOWER_THAN_ELSE { $$ = new IfStmt($3, $5, nullptr); }
            | T_If '(' Expr ')' Stmt T_Else Stmt           { $$ = new IfStmt($3, $5, $7); }
            | T_While '(' Expr ')' Stmt  { $$ = new WhileStmt($3, $5); }
            | T_Do Stmt T_While '(' Expr ')' ';' { $$ = new DoWhileStmt($2, $5); }
            | T_For '(' ForInit OptExpr ';' OptExpr ')' Stmt { $$ = new ForStmt($3, $4, $6, $8); }
            | T_Switch '(' Expr ')' Block { $$ = new SwitchStmt($3, $5); }
            | T_Case CondExpr ':'        { $$ = new CaseLabel($2); }
            | T_Default ':'              { $$ = new CaseLabel(nullptr); }
            | T_Return OptExpr ';'       { $$ = new ReturnStmt($2); }
            | T_Break ';'                { $$ = new BreakStmt(); }
            | T_Continue ';'             { $$ = new ContinueStmt(); }
            ;

ForInit     : DeclStmt                   { $$ = $1; }
            | OptExpr ';'                { $$ = $1 ? (Stmt *)new ExprStmt($1) : (Stmt *)new Block(); }
            ;

OptExpr     : Expr                       { $$ = $1; }
            | /* empty */                { $$ = nullptr; }
            ;

Expr        : AssignExpr                 { $$ = $1; }
            ;

AssignExpr  : CondExpr                   { $$ = $1; }
            | UnaryExpr '=' AssignExpr   { $$ = new AssignExpr(0, $1, $3); }
            | UnaryExpr T_OpAssign AssignExpr { $$ = new AssignExpr($2, $1, $3); }
            ;

CondExpr    : OrExpr                     { $$ = $1; }
            | OrExpr '?' Expr ':' CondExpr { $$ = new CondExprNode($1, $3, $5); }
            ;

OrExpr      : AndExpr                    { $$ = $1; }
            | OrExpr T_Or AndExpr        { $$ = new LogicalExpr('|', $1, $3); }
            ;
AndExpr     : BitOrExpr                  { $$ = $1; }
            | AndExpr T_And BitOrExpr    { $$ = new LogicalExpr('&', $1, $3); }
            ;
BitOrExpr   : XorExpr                    { $$ = $1; }
            | BitOrExpr '|' XorExpr      { $$ = new BinaryExpr('|', $1, $3); }
            ;
XorExpr     : BitAndExpr                 { $$ = $1; }
            | XorExpr '^' BitAndExpr     { $$ = new BinaryExpr('^', $1, $3); }
            ;
BitAndExpr  : EqExpr                     { $$ = $1; }
            | BitAndExpr '&' EqExpr      { $$ = new BinaryExpr('&', $1, $3); }
            ;
EqExpr      : RelExpr                    { $$ = $1; }
            | EqExpr T_Eq RelExpr        { $$ = new CompareExpr(Cond::EQ, $1, $3); }
            | EqExpr T_Ne RelExpr        { $$ = new CompareExpr(Cond::NE, $1, $3); }
            ;
RelExpr     : ShiftExpr                  { $$ = $1; }
            | RelExpr '<' ShiftExpr      { $$ = new CompareExpr(Cond::LT, $1, $3); }
            | RelExpr '>' ShiftExpr      { $$ = new CompareExpr(Cond::GT, $1, $3); }
            | RelExpr T_Le ShiftExpr     { $$ = new CompareExpr(Cond::LE, $1, $3); }
            | RelExpr T_Ge ShiftExpr     { $$ = new CompareExpr(Cond::GE, $1, $3); }
            ;
ShiftExpr   : AddExpr                    { $$ = $1; }
            | ShiftExpr T_Shl AddExpr    { $$ = new BinaryExpr('L', $1, $3); }
            | ShiftExpr T_Shr AddExpr    { $$ = new BinaryExpr('R', $1, $3); }
            ;
AddExpr     : MulExpr                    { $$ = $1; }
            | AddExpr '+' MulExpr        { $$ = new BinaryExpr('+', $1, $3); }
            | AddExpr '-' MulExpr        { $$ = new BinaryExpr('-', $1, $3); }
            ;
MulExpr     : UnaryExpr                  { $$ = $1; }
            | MulExpr '*' UnaryExpr      { $$ = new BinaryExpr('*', $1, $3); }
            | MulExpr '/' UnaryExpr      { $$ = new BinaryExpr('/', $1, $3); }
            | MulExpr '%' UnaryExpr      { $$ = new BinaryExpr('%', $1, $3); }
            ;
UnaryExpr   : PostfixExpr                { $$ = $1; }
            | '-' UnaryExpr              { $$ = new UnaryExprNode('-', $2); }
            | '+' UnaryExpr              { $$ = new UnaryExprNode('+', $2); }
            | '~' UnaryExpr              { $$ = new UnaryExprNode('~', $2); }
            | '!' UnaryExpr              { $$ = new UnaryExprNode('!', $2); }
            | T_Inc UnaryExpr            { $$ = new IncDecExpr(true, '+', $2); }
            | T_Dec UnaryExpr            { $$ = new IncDecExpr(true, '-', $2); }
            | '(' TypeSpec ')' UnaryExpr { $$ = new CastExpr(*$2, $4); }
            ;
PostfixExpr : PrimaryExpr                { $$ = $1; }
            | T_Identifier '(' Args ')'  { $$ = new CallExpr($1, *$3); }
            | PostfixExpr '.' T_Identifier { $$ = new MemberExpr($1, $3); }
            | PostfixExpr T_Inc          { $$ = new IncDecExpr(false, '+', $1); }
            | PostfixExpr T_Dec          { $$ = new IncDecExpr(false, '-', $1); }
            ;
PrimaryExpr : T_IntConst                 { $$ = new IntLit($1.value, Type($1.bits, !$1.isUnsigned)); }
            | T_Identifier               { $$ = new VarRef($1); }
            | '(' Expr ')'               { $$ = $2; }
            ;

Args        : ArgList                    { $$ = $1; }
            | /* empty */                { $$ = new std::vector<Expr *>(); }
            ;
ArgList     : AssignExpr                 { ($$ = new std::vector<Expr *>())->push_back($1); }
            | ArgList ',' AssignExpr     { ($$ = $1)->push_back($3); }
            ;

%%
