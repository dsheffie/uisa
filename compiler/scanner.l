/* scanner.l: the C-subset scanner (forked from Decaf's scanner.l). */

%{
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include "ast.h"
#include "errors.h"
#include "parser.hpp"
%}

%option noyywrap nounput noinput yylineno
%x COMM

DIGIT             [0-9]
HEX_DIGIT         [0-9a-fA-F]
IDENTIFIER        [a-zA-Z_][a-zA-Z_0-9]*

%%

[ \t\r\n]+             { }
"/*"                   { BEGIN(COMM); }
<COMM>"*/"             { BEGIN(INITIAL); }
<COMM>\n               { }
<COMM>.                { }
<COMM><<EOF>>          { error(yylineno, "unterminated comment"); return 0; }
"//"[^\n]*             { }

 /* --------------------- Keywords ------------------------------- */
"void"              { return T_Void; }
"u8"                { yylval.type = new Type(8, false); return T_Type; }
"u16"               { yylval.type = new Type(16, false); return T_Type; }
"u32"               { yylval.type = new Type(32, false); return T_Type; }
"s8"                { yylval.type = new Type(8, true); return T_Type; }
"s16"               { yylval.type = new Type(16, true); return T_Type; }
"s32"               { yylval.type = new Type(32, true); return T_Type; }
"u64"               { yylval.type = new Type(64, false); return T_Type; }
"s64"               { yylval.type = new Type(64, true); return T_Type; }
"int"               { yylval.type = new Type(32, true); return T_Type; }
"unsigned"          { yylval.type = new Type(32, false); return T_Type; }
"const"             { return T_Const; }
"entry"             { return T_Entry; }
"while"             { return T_While; }
"do"                { return T_Do; }
"for"               { return T_For; }
"if"                { return T_If; }
"else"              { return T_Else; }
"switch"            { return T_Switch; }
"case"              { return T_Case; }
"default"           { return T_Default; }
"return"            { return T_Return; }
"break"             { return T_Break; }
"continue"          { return T_Continue; }
"float"|"double"|"long"|"char"|"short"|"struct"|"union"|"goto"|"static" {
                      error(yylineno, "'%s' is not part of the millicode C subset", yytext); }

 /* -------------------- Operators ----------------------------- */
"<="                { return T_Le; }
">="                { return T_Ge; }
"=="                { return T_Eq; }
"!="                { return T_Ne; }
"&&"                { return T_And; }
"||"                { return T_Or; }
"<<"                { return T_Shl; }
">>"                { return T_Shr; }
"++"                { return T_Inc; }
"--"                { return T_Dec; }
"+="|"-="|"*="|"/="|"%="|"&="|"|="|"^=" { yylval.op = yytext[0]; return T_OpAssign; }
"<<="               { yylval.op = 'L'; return T_OpAssign; }
">>="               { yylval.op = 'R'; return T_OpAssign; }
[-+*/%=,;!~<>()\[\]{}:?&|^] { return yytext[0]; }

 /* -------------------- Constants ------------------------------ */
(0[xX]{HEX_DIGIT}+|{DIGIT}+)([uU]|[uU]?[lL][lL]|[lL][lL][uU])? {
                      /* C's rules with s32/u32/s64/u64 standing in for int/unsigned/long long:
                         the first type that holds the value, unsigned types only for hex
                         and octal constants or with a u suffix; ll forces 64 bits */
                      char *end;
                      errno = 0;
                      unsigned long long v = strtoull(yytext, &end, 0);
                      if (errno == ERANGE) error(yylineno, "integer constant %s does not fit 64 bits", yytext);
                      bool decimal = yytext[0] != '0' || yytext[1] == 0;
                      bool u = strchr(end, 'u') || strchr(end, 'U');
                      bool ll = strchr(end, 'l') || strchr(end, 'L');
                      int bits = 32;
                      bool isU = false;
                      if (!ll && !u && v <= 0x7fffffffull) bits = 32;
                      else if (!ll && v <= 0xffffffffull && (u || !decimal)) isU = true;
                      else if (!u && v <= 0x7fffffffffffffffull) bits = 64;
                      else if (u || !decimal) { bits = 64; isU = true; }
                      else error(yylineno, "integer constant %s does not fit s64", yytext);
                      if (ll && bits == 32) bits = 64;
                      yylval.lit.value = v;
                      yylval.lit.bits = bits;
                      yylval.lit.isUnsigned = isU;
                      return T_IntConst; }

{IDENTIFIER}        { yylval.ident = strdup(yytext); return T_Identifier; }
.                   { error(yylineno, "unexpected character '%c'", yytext[0]); }

%%
