/* scanner.l: the C-subset scanner (forked from Decaf's scanner.l). */

%{
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "ast.h"
#include "errors.h"
#include "parser.hpp"
%}

%{
// #include "file": a stack of buffers; each file is read at most once.
struct IncludeFrame { YY_BUFFER_STATE buf; const char *file; int vline; };
static std::vector<IncludeFrame> g_incStack;
static std::set<std::string> g_included;
static const char *g_curFile = nullptr;
static void beginInclude(const char *text);
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
^[ \t]*"#"[ \t]*"include"[ \t]*\"[^"\n]*\"[^\n]* { beginInclude(yytext); }
^[ \t]*"#"[ \t]*"pragma"[ \t]+"once"[^\n]* { /* every file is read at most once anyway */ }
"#"[^\n]*             { error(yylineno, "only #include \"file\" is supported, not '%s'", yytext); }
<<EOF>>             {
                      if (g_incStack.empty()) yyterminate();
                      IncludeFrame fr = g_incStack.back();
                      g_incStack.pop_back();
                      yy_delete_buffer(YY_CURRENT_BUFFER);
                      yy_switch_to_buffer(fr.buf);
                      g_curFile = fr.file;
                      // the rest of the #include line, then the including file's next line
                      srcSegment(yylineno, fr.file, fr.vline);
                    }

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
"struct"            { return T_Struct; }
"typedef"           { return T_Typedef; }
"inline"            { return T_Inline; }
"goto"              { return T_Goto; }
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
"float"|"double"|"long"|"char"|"short"|"union"|"static" {
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
[-+*/%=,;!~<>()\[\]{}:?&|^.] { return yytext[0]; }

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

{IDENTIFIER}        {
                      auto it = g_typeNames.find(yytext);
                      if (it != g_typeNames.end()) {
                        yylval.type = new Type(it->second);
                        return T_Type;
                      }
                      yylval.ident = strdup(yytext);
                      return T_Identifier; }
.                   { error(yylineno, "unexpected character '%c'", yytext[0]); }

%%

static void beginInclude(const char *text) {
  const char *q = strchr(text, '"');
  std::string name(q + 1, strchr(q + 1, '"'));
  if (!g_curFile) g_curFile = g_srcName;
  std::string path = name;
  if (name[0] != '/') {
    std::string cur = g_curFile;
    size_t slash = cur.rfind('/');
    if (slash != std::string::npos) path = cur.substr(0, slash + 1) + name;
  }
  char real[4096];
  std::string key = realpath(path.c_str(), real) ? std::string(real) : path;
  if (g_included.empty()) {
    char r0[4096];
    g_included.insert(realpath(g_srcName, r0) ? std::string(r0) : std::string(g_srcName));
  }
  if (!g_included.insert(key).second) return;   // already included once
  FILE *f = fopen(path.c_str(), "r");
  if (!f) {
    error(yylineno, "cannot open include file '%s'", path.c_str());
    return;
  }
  g_incStack.push_back({YY_CURRENT_BUFFER, g_curFile, srcLine(yylineno)});
  g_curFile = strdup(path.c_str());
  srcSegment(yylineno, g_curFile, 1);
  yy_switch_to_buffer(yy_create_buffer(f, YY_BUF_SIZE));
}
