// -*-c++-*-
/* errors.h: diagnostics (replaces Decaf's ReportError class). */
#ifndef UCC_ERRORS_H
#define UCC_ERRORS_H

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

extern const char *g_srcName;
extern int g_errors;

#if defined(__GNUC__)
#define UCC_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define UCC_PRINTF(a, b)
#endif

// Source lines are one global count across #include files (yylineno never
// restarts); srcFile/srcLine map such a line back to its file and local line.
void srcSegment(int vline, const char *file, int localLine);
const char *srcFile(int vline);
int srcLine(int vline);

void error(int line, const char *fmt, ...) UCC_PRINTF(2, 3);
[[noreturn]] void fatal(int line, const char *fmt, ...) UCC_PRINTF(2, 3);

#endif
