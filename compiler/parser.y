/* parser.y: grammar of the millicode C subset.
 *
 * C's statements and expressions over the integer types u8 u16 u32 u64 s8
 * s16 s32 s64 (int = s32, unsigned = u32), plain structs, typedefs, inline
 * functions with reference parameters; program mode adds pointers, arrays,
 * globals and C's integer keywords.
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
// `unsigned long`, `long long int`, `signed char`, ...: C's keyword
// combinations (program mode: long is 64 bits, as on the LP64 host).
static Type *combineSpec(Type *a, Type *b) {
  int k = a->kw, add = b->kw;
  if (!k || !add) {
    error(yylineno, "two types in one declaration");
    return a;
  }
  if ((k & 16) && (add & 16)) add = 32;
  else if (k & add) error(yylineno, "repeated type keyword");
  k |= add;
  if (((k & 1) && (k & 2)) || ((k & 8) && (k & 48)) || ((k & 64) && (k & 60)))
    error(yylineno, "conflicting type keywords");
  Type *t = new Type((k & 64) ? 8 : (k & 8) ? 16 : (k & 48) ? 64 : 32, !(k & 1));
  t->kw = k;
  return t;
}
static Type ptrs(Type t, int n) {
  for (int i = 0; i < n; i++) t = Type::ptrTo(t);
  return t;
}
// An array field's dimension: an integer constant expression (literals and
// arithmetic on them; macros arrive here already expanded).
static bool constDim(Expr *e, int64_t &v) {
  if (auto *i = dynamic_cast<IntLit *>(e)) {
    v = (int64_t)i->value;
    return true;
  }
  if (auto *u = dynamic_cast<UnaryExprNode *>(e)) {
    if (!constDim(u->e, v)) return false;
    if (u->op == '-') v = -v;
    else if (u->op == '~') v = ~v;
    else if (u->op != '+') return false;
    return true;
  }
  if (auto *b = dynamic_cast<BinaryExpr *>(e)) {
    int64_t x, y;
    if (!constDim(b->l, x) || !constDim(b->r, y)) return false;
    switch (b->op) {
      case '+': v = x + y; return true;
      case '-': v = x - y; return true;
      case '*': v = x * y; return true;
      case '/': if (!y) return false; v = x / y; return true;
      case '%': if (!y) return false; v = x % y; return true;
      case 'L': v = x << (y & 63); return true;
      case 'R': v = x >> (y & 63); return true;
    }
  }
  return false;
}
static void addField(const DeclItem &d, const Type &t) {
  Type ft = ptrs(t, d.ptrs);
  if (d.ptrs && !g_program) error(d.line, "pointers: only available with --program");
  if (!d.dims.empty() && !g_program) error(d.line, "arrays: only available with --program");
  for (int k = (int)d.dims.size() - 1; k >= 0 && g_program; k--) {
    int64_t n = 0;
    if (!d.dims[k] || !constDim(d.dims[k], n) || n <= 0 || n > 0x1000000) {
      error(d.line, "array field '%s' needs a positive constant dimension", d.name);
      n = 1;
    }
    if (ft.isVoid) error(d.line, "array of void");
    ft = Type::arrayOf(ft, (int)n);
  }
  g_curStruct->fields.push_back({d.name, ft});
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
  DeclItem *decl;
  InitList *ilist;
  FnDecl *fn;
  TransUnit *tu;
}

%token T_Extern T_Asm T_Arrow T_Sizeof
%token <ident> T_String
%token T_Void T_Const T_Entry T_Struct T_Typedef T_Inline T_Goto T_While T_Do T_For T_If T_Else T_Switch T_Case T_Default
%token T_Return T_Break T_Continue T_Le T_Ge T_Eq T_Ne T_And T_Or T_Shl T_Shr T_Inc T_Dec
%token <type> T_Type
%token <op> T_OpAssign
%token <lit> T_IntConst
%token <ident> T_Identifier

%type <tu> TransUnit
%type <fn> FunctionDecl
%type <type> RetType TypeSpec CastType
%type <op> Pointers
%type <decl> DeclItem Declarator
%type <ilist> InitItems
%type <exprs> Dims
%type <params> Params ParamList
%type <block> Block Stmts
%type <stmt> Stmt DeclStmt ForInit
%type <decls> DeclItems
%type <expr> Expr OptExpr AssignExpr CondExpr OrExpr AndExpr BitOrExpr XorExpr BitAndExpr
%type <expr> EqExpr RelExpr ShiftExpr AddExpr MulExpr UnaryExpr NoCastExpr PostfixExpr PrimaryExpr
%type <exprs> Args ArgList

%nonassoc LOWER_THAN_ELSE
%nonassoc T_Else

%%

Program     : TransUnit                  { parse_result = $1; }
            ;

TransUnit   : TransUnit FunctionDecl     { ($$ = $1)->fns.push_back($2); }
            | TransUnit TypeSpec DeclItems ';'
                                         { ($$ = $1)->globals.push_back(new GlobalDecl(*$2, *$3)); }
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
FieldNames  : Declarator                 { addField(*$1, *$<type>0); }
            | FieldNames ',' Declarator  { addField(*$3, *$<type>0); }
            ;

TypeSpec    : T_Type                     { $$ = $1; }
            | TypeSpec T_Type            { $$ = combineSpec($1, $2); }
            | T_Struct T_Type            { $$ = $2; if (!$2->isStruct()) error(yylineno, "'struct' before a type that is not a struct"); }
            ;

FunctionDecl: TypeSpec Pointers T_Identifier '(' Params ')' Block
                                         { $$ = new FnDecl(ptrs(*$1, $2), $3, *$5, $7, false); }
            | T_Void Pointers T_Identifier '(' Params ')' Block
                                         { $$ = new FnDecl(ptrs(Type::voidType(), $2), $3, *$5, $7, false); }
            | TypeSpec Pointers T_Identifier '(' Params ')' ';'
                                         { $$ = new FnDecl(ptrs(*$1, $2), $3, *$5, nullptr, false); $$->proto = true; }
            | T_Void Pointers T_Identifier '(' Params ')' ';'
                                         { $$ = new FnDecl(ptrs(Type::voidType(), $2), $3, *$5, nullptr, false); $$->proto = true; }
            | T_Inline RetType T_Identifier '(' Params ')' Block
                                         { $$ = new FnDecl(*$2, $3, *$5, $7, false); $$->isInline = true; }
            | T_Entry RetType T_Identifier '(' Params ')' Block
                                         { $$ = new FnDecl(*$2, $3, *$5, $7, true); }
            | T_Extern RetType T_Identifier '(' Params ')' T_Asm '(' T_String ')' ';'
                                         { $$ = new FnDecl(*$2, $3, *$5, nullptr, false); $$->asmSpec = $9; }
            ;

RetType     : TypeSpec                   { $$ = $1; }
            | T_Void                     { $$ = new Type(Type::voidType()); }
            ;

Params      : ParamList                  { $$ = $1; }
            | T_Void                     { $$ = new std::vector<Param>(); }
            | /* empty */                { $$ = new std::vector<Param>(); }
            ;

ParamList   : TypeSpec Declarator        { ($$ = new std::vector<Param>())->push_back(Param{ptrs(*$1, $2->ptrs + !$2->dims.empty()), $2->name, false}); }
            | T_Void '*' Declarator      { ($$ = new std::vector<Param>())->push_back(Param{ptrs(Type::voidType(), 1 + $3->ptrs + !$3->dims.empty()), $3->name, false}); }
            | TypeSpec '&' T_Identifier  { ($$ = new std::vector<Param>())->push_back(Param{*$1, $3, true}); }
            | ParamList ',' TypeSpec Declarator { ($$ = $1)->push_back(Param{ptrs(*$3, $4->ptrs + !$4->dims.empty()), $4->name, false}); }
            | ParamList ',' T_Void '*' Declarator { ($$ = $1)->push_back(Param{ptrs(Type::voidType(), 1 + $5->ptrs + !$5->dims.empty()), $5->name, false}); }
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

DeclItems   : DeclItem                   { ($$ = new std::vector<DeclItem>())->push_back(*$1); }
            | DeclItems ',' DeclItem     { ($$ = $1)->push_back(*$3); }
            ;

DeclItem    : Declarator                 { $$ = $1; }
            | Declarator '=' AssignExpr  { ($$ = $1)->init = $3; }
            | Declarator '=' '{' InitItems OptComma '}' { ($$ = $1)->list = $4; }
            ;

/* `**name[3][4]`: pointers and array dimensions (program mode) */
Declarator  : Pointers T_Identifier Dims { $$ = new DeclItem{$2, nullptr, yylineno}; $$->ptrs = $1; $$->dims = *$3; }
            ;
Pointers    : /* empty */                { $$ = 0; }
            | Pointers '*'               { $$ = $1 + 1; }
            | Pointers '*' T_Const       { $$ = $1 + 1; }
            ;
Dims        : /* empty */                { $$ = new std::vector<Expr *>(); }
            | Dims '[' CondExpr ']'      { ($$ = $1)->push_back($3); }
            | Dims '[' ']'               { ($$ = $1)->push_back(nullptr); }
            ;
InitItems   : AssignExpr                 { ($$ = new InitList())->items.push_back({$1, nullptr}); }
            | '{' InitItems OptComma '}' { ($$ = new InitList())->items.push_back({nullptr, $2}); }
            | InitItems ',' AssignExpr   { ($$ = $1)->items.push_back({$3, nullptr}); }
            | InitItems ',' '{' InitItems OptComma '}' { ($$ = $1)->items.push_back({nullptr, $4}); }
            ;
OptComma    : /* empty */ | ',' ;

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
            | T_Goto T_Identifier ';'    { $$ = new GotoStmt($2); }
            | T_Identifier ':' Stmt      { $$ = new LabelStmt($1, $3); }
            | T_Continue ';'             { $$ = new ContinueStmt(); }
            ;

ForInit     : DeclStmt                   { $$ = $1; }
            | OptExpr ';'                { $$ = $1 ? (Stmt *)new ExprStmt($1) : (Stmt *)new Block(); }
            ;

OptExpr     : Expr                       { $$ = $1; }
            | /* empty */                { $$ = nullptr; }
            ;

Expr        : AssignExpr                 { $$ = $1; }
            | Expr ',' AssignExpr        { $$ = new CommaExpr($1, $3); }
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
UnaryExpr   : NoCastExpr                 { $$ = $1; }
            | '(' CastType ')' UnaryExpr { $$ = new CastExpr(*$2, $4); }
            ;
/* C's unary-expression: what sizeof applies to (a cast is not one) */
NoCastExpr  : PostfixExpr                { $$ = $1; }
            | T_Sizeof NoCastExpr        { $$ = new SizeofExpr($2); }
            | T_Sizeof '(' CastType Dims ')' { $$ = new SizeofExpr(*$3, *$4); }
            | '-' UnaryExpr              { $$ = new UnaryExprNode('-', $2); }
            | '+' UnaryExpr              { $$ = new UnaryExprNode('+', $2); }
            | '~' UnaryExpr              { $$ = new UnaryExprNode('~', $2); }
            | '!' UnaryExpr              { $$ = new UnaryExprNode('!', $2); }
            | T_Inc UnaryExpr            { $$ = new IncDecExpr(true, '+', $2); }
            | T_Dec UnaryExpr            { $$ = new IncDecExpr(true, '-', $2); }
            | '&' UnaryExpr              { $$ = new AddrOfExpr($2); }
            | '*' UnaryExpr              { $$ = new DerefExpr($2); }
            ;
CastType    : TypeSpec Pointers          { $$ = new Type(ptrs(*$1, $2)); }
            | T_Void Pointers            { $$ = new Type(ptrs(Type::voidType(), $2)); }
            ;
PostfixExpr : PrimaryExpr                { $$ = $1; }
            | T_Identifier '(' Args ')'  { $$ = new CallExpr($1, *$3); }
            | PostfixExpr '.' T_Identifier { $$ = new MemberExpr($1, $3); }
            | PostfixExpr T_Arrow T_Identifier { $$ = new MemberExpr(new DerefExpr($1), $3); }
            | PostfixExpr '[' Expr ']'   { $$ = new IndexExpr($1, $3); }
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
