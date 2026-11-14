/* File: main.cpp
 *
 * This file defines the main() routine for the program and not much else.
 */

#include "errors.h"
#include "manual_token.h"
#include "options.h"
#include "parser.h"
#include "tac.h"

/**
 * Prints information about a scanned token.
 */
void token_print(int token, const char *text, YYSTYPE val, yyltype loc);

/**
 * Initializes the compiler.
 */
void init() {
  scanner_init();
  parser_init();
  llvm::InitializeNativeTarget();
  llvm::InitializeAllTargetMCs(); 
  llvm::InitializeNativeTargetAsmPrinter(); 
}

/**
 * yylex() is then repeatedly called until EOF is reached; each token is printed
 * as it is lexed.
 */
void print_all_tokens(){
  int token;
  while( (token = yylex()) != 0 )
    token_print( token, yytext, yylval, yylloc );
}

/**
 * Entry point to the entire program.  We parse the command line and initialize
 * the scanner.
 *
 * @return 0 if requested action succeeded, -1 if an error was detected, 1 if
 * mode not yet implemented
 */
int main(int argc, char *argv[]) {
  options.parse( argc, argv );
  init();
  
  if( options.getOutputType() == OUTPUT_LEX_TOKENS )
    print_all_tokens();
  else {
    yyparse();
    if( parser_get_result() ) {
      switch( options.getOutputType() ) {
      case OUTPUT_PARSE_TREE:
	parser_get_result()->print(0);
	break;
	      
      case OUTPUT_CHECK_ONLY:
	parser_get_result()->check();
	break;
	    
      case OUTPUT_TAC:
      case OUTPUT_MIPS:
	//need check to generate type information and
	//scope information
	if(parser_get_result()->check()){
	  //only generate code if no semantic errors
	  std::string foo;
	  parser_get_result()->emitprog(foo);
	}
	break;
      case OUTPUT_CPP:
      default:
	fprintf( stderr, "Output mode '%s' is not yet implemented.\n",
		 output_type_to_string(options.getOutputType()) );
	exit( 1 );
      }
    }
  }
  
    llvm::llvm_shutdown();
    return( ReportError::NumErrors()==0 ? 0 : -1 );
}

/**
 * Prints information about a scanned token.
 */
void token_print(int token, const char *text, YYSTYPE val, yyltype loc)
{
    char buffer[8];
    snprintf( buffer, 8, "'%c'", (char)token );
    const char *name = token >= T_Void ? token_to_string(token) : buffer;

    printf( "%-12s line %d cols %d-%d is %s ",
            text, loc.first_line, loc.first_column, loc.last_column, name );

    switch(token) {
    case T_IntConst:
        printf("(value = %d)\n", val.intConst);
        break;

    case T_DblConst:
        printf("(value = %g)\n", val.dblConst);
        break;

    case T_StrConst:
        printf("(value = %s)\n", val.strConst);
        break;

    case T_BoolConst:
        printf("(value = %s)\n", val.boolConst ? "true" : "false");
        break;

    case T_Identifier:
        if (strcmp(text, val.identifier)) {
            printf("(truncated to %s)\n", val.identifier);
            break;
        }

    default:
        printf("\n");
        break;
    }
}
