/* File:  scanner.l

 * Lex inupt file to generate the scanner for the compiler.
 */

%{

#include <string.h>
#include "debug.h" /* for die() and debug_print() */
#include "errors.h"
#include "scanner.h"
#include "parser.h"
#include "list.h"

#define TAB_SIZE 8

static int curLineNum, curColNum;
List<char*> savedLines;

static char *MakeEscapedString(char *raw, int len);
static void do_before_each_action();
#define YY_USER_ACTION do_before_each_action();

%}

/* States
 *
 * A little wrinkle on states is the COPY exclusive state which
 * I added to first match each line and copy it ot the list of lines
 * read before re-processing it. This allows us to print the entire
 * line later to provide context on errors.
 */
%s N
%x COMM COPY
%option stack

/* Definitions
 *
 * To make our rules more readable, we establish some definitions here.
 */
DIGIT             ([0-9])
HEX_DIGIT         ([0-9a-fA-F])
HEX_INTEGER       (0[Xx]{HEX_DIGIT}+)
INTEGER           ({DIGIT}+)
EXPONENT          ([Ee][-+]?{INTEGER})
DOUBLE            ({INTEGER}"."{DIGIT}*{EXPONENT}?)
ANY_CHAR          ([.\n"])
ESCAPE_SEQUENCE   (\\{ANY_CHAR})
BEG_STRING        (\"({ESCAPE_SEQUENCE}|[^"\n])*)
STRING            ({BEG_STRING}\")
IDENTIFIER        ([a-zA-Z][a-zA-Z_0-9]*)
OPERATOR          ([-+/*%=.,;!<>()[\]{}:])
BEG_COMMENT       ("/*")
END_COMMENT       ("*/")
SINGLE_COMMENT    ("//"[^\n]*)

%%             /* BEGIN RULES SECTION */


<COPY>.*               { char curLine[512];
                         savedLines.append(strdup(yytext));
                         curColNum = 1; yy_pop_state(); yyless(0); }
<COPY><<EOF>>          { yy_pop_state(); }
<*>\n                  { curLineNum++; curColNum = 1;
                         if (YYSTATE == COPY) savedLines.append("");
                         else yy_push_state(COPY); }
[ ]+                   { /* ignore all spaces */  }
<*>[\t]                { curColNum += TAB_SIZE - curColNum%TAB_SIZE + 1; }

 /* -------------------- Comments ----------------------------- */
{BEG_COMMENT}          { BEGIN(COMM); }
<COMM>{END_COMMENT}    { BEGIN(N); }
<COMM><<EOF>>          { ReportError::UntermComment();
                         return 0; }
<COMM>.                { /* ignore everything else that doesn't match */ }
{SINGLE_COMMENT}       { /* skip to end of line for // comment */ }


 /* --------------------- Keywords ------------------------------- */
"void"              { return T_Void;        }
"int"               { return T_Int;         }
"double"            { return T_Double;      }
"bool"              { return T_Bool;        }
"string"            { return T_String;      }
"null"              { return T_Null;        }
"class"             { return T_Class;       }
"extends"           { return T_Extends;     }
"this"              { return T_This;        }
"interface"         { return T_Interface;   }
"implements"        { return T_Implements;  }
"while"             { return T_While;       }
"for"               { return T_For;         }
"if"                { return T_If;          }
"else"              { return T_Else;        }
"switch"            { return T_Switch;      }
"case"              { return T_Case;        }
"default"           { return T_Default;     }
"return"            { return T_Return;      }
"break"             { return T_Break;       }
"New"               { return T_New;         }
"NewArray"          { return T_NewArray;    }
"ReadInteger"       { return T_ReadInteger; }
"ReadLine"          { return T_ReadLine;    }
"Print"             { return T_Print;       }
"Malloc"            { return T_Malloc;      }

 /* -------------------- Operators ----------------------------- */
"<="                { return T_LessEqual;   }
">="                { return T_GreaterEqual;}
"=="                { return T_Equal;       }
"!="                { return T_NotEqual;    }
"&&"                { return T_And;         }
"||"                { return T_Or;          }
"[]"                { return T_Dims;        }
"++"                { return T_PlusPlus;    }
"--"                { return T_MinusMinus;  }
{OPERATOR}          { return yytext[0];     }

 /* -------------------- Constants ------------------------------ */
"true"|"false"      { yylval.boolConst = (yytext[0] == 't');
                         return T_BoolConst; }
{INTEGER}           { yylval.intConst = strtol(yytext, NULL, 10);
                         return T_IntConst; }
{HEX_INTEGER}       { yylval.intConst = strtol(yytext, NULL, 16);
                         return T_IntConst; }
{DOUBLE}            { yylval.dblConst = atof(yytext);
                         return T_DblConst; }
{STRING}            { yylval.strConst = strdup(yytext);
                      return T_StrConst; }
{BEG_STRING}        { ReportError::UntermString(&yylloc,yytext); }


 /* -------------------- Identifiers --------------------------- */
{IDENTIFIER}        { if (strlen(yytext) > MaxIdentLen)
                         ReportError::LongIdentifier(&yylloc, yytext);
                       strncpy(yylval.identifier, yytext, MaxIdentLen);
                       yylval.identifier[MaxIdentLen] = '\0';
                       return T_Identifier; }


 /* -------------------- Default rule (error) -------------------- */
.                   { ReportError::UnrecogChar(&yylloc, yytext[0]); }

%%

void scanner_init()
{
    yy_flex_debug = false;
    BEGIN(N);
    yy_push_state(COPY); // copy first line at start
    curLineNum = 1;
    curColNum = 1;
}


static void do_before_each_action()
{
   yylloc.first_line = curLineNum;
   yylloc.last_line = curLineNum;
   yylloc.first_column = curColNum;
   yylloc.last_column = curColNum + yyleng - 1;
   curColNum += yyleng;
}

const char *GetLineNumbered(int num) {
   if (num <= 0 || num > savedLines.size()) return NULL;
   return savedLines[num-1];
}
