/* File: parser.h
 *
 * This file provides constants and type definitions that will
 * are used and/or exported by the yacc-generated parser.
 */

#ifndef PARSER_H
#define PARSER_H

// include things needed for the yylval union
// (types, classes, constants, etc.)

#include "scanner.h"      // for MaxIdentLen
#include "list.h"         // because we use all these types
#include "ast.h"          // in the union, we need their declarations
#include "ast_type.h"
#include "ast_decl.h"
#include "ast_expr.h"
#include "ast_stmt.h"


// Next, we want to get the exported defines for the token codes and
// typedef for YYSTYPE and exported global variable yylval.  These
// definitions are generated and written to the y.tab.h header file. But
// because that header does not have any protection against being
// re-included and those definitions are also present in the y.tab.c,
// we can get into trouble if we don't take precaution to not include if
// we are compiling y.tab.c, which we use the YYBISON symbol for.
#ifndef YYBISON
#include "y.tab.h"
#endif

/**
 * Method defined defined in the bison-generated y.tab.c file for parsing an
 * input from standard in.
 */
int yyparse();

/**
 * Student-defined method to initialize the parser internals.
 */
void parser_init();

/**
 * Returns the Program object which resulted from the parse.
 */
Program* parser_get_result();

#endif /* PARSER_H */
