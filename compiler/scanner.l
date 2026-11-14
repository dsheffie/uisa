/* scanner.l: the C-subset scanner (forked from Decaf's scanner.l). */

%{
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
(0[xX]{HEX_DIGIT}+|{DIGIT}+)[uU]? {
                      char *end;
                      unsigned long long v = strtoull(yytext, &end, 0);
                      bool hex = yytext[0] == '0' && (yytext[1] == 'x' || yytext[1] == 'X');
                      bool u = *end == 'u' || *end == 'U';
                      if (v > 0xffffffffull) error(yylineno, "integer constant %s does not fit 32 bits", yytext);
                      yylval.lit.value = (uint32_t)v;
                      yylval.lit.isUnsigned = u || v > 0x7fffffffull;
                      (void)hex;
                      return T_IntConst; }

{IDENTIFIER}        { yylval.ident = strdup(yytext); return T_Identifier; }
.                   { error(yylineno, "unexpected character '%c'", yytext[0]); }

%%
