/* File: location.h
 *
 * This file just contains features relative to the location structure used to
 * record the lexical position of a token or symbol.
 */

#ifndef LOCATION_H
#define LOCATION_H

#include "manual_token.h"

#define MAX_LINE_LENGTH 10000 /* just an assumed value, not guarunteed */

/**
 * Takes two locations and returns a new location which represents
 * the span from first one to the last one (whatever order that may be).
 */
inline yyltype loc_join(yyltype one, yyltype two)
{
  yyltype combined;

  /* determine which comes first */
  if( one.first_line<two.first_line || (one.first_line==two.first_line && one.first_column<two.first_column) ) {
    combined.first_line   = one.first_line;
    combined.first_column = one.first_column;
  }
  else {
    combined.first_line   = two.first_line;
    combined.first_column = two.first_column;
  }

  /* determine which comes last */
  if( one.last_line>two.last_line || (one.last_line==two.last_line && one.last_column>two.last_column) ) {
    combined.last_line   = one.last_line;
    combined.last_column = one.last_column;
  }
  else {
    combined.last_line   = two.last_line;
    combined.last_column = two.last_column;
  }

  return combined;
}

/** Same as above, except operates on pointers as a convenience  */
inline yyltype loc_join(yyltype *onePtr, yyltype *twoPtr)
{
  return loc_join(*onePtr, *twoPtr);
}

/** Returns how many lines loc2 is from loc1.  Positive indicates that loc1 is earlier. */
inline int loc_lineDiff(yyltype loc1, yyltype loc2) {
  return( loc2.first_line - loc1.last_line );
}

/** Returns how many columns loc2 is from loc1.  Positive indicates that loc1 is earlier. */
inline int loc_columnDiff(yyltype loc1, yyltype loc2) {
  return( loc2.first_column - loc1.last_column );
}

/** Determines whether loc1 is before loc2.  Returns 1 if loc1 is before loc2 or on the same line. */
inline int loc_before(yyltype loc1, yyltype loc2) {
  return( loc_lineDiff(loc1, loc2) >= 0 );
}

inline int loc_before(yyltype *loc1, yyltype *loc2) {
    return loc_before(*loc1, *loc2);
}

enum CLOSENESS { CLSR_NEITHER = 1, CLSR_LOC1, CLSR_LOC2 };

/**
 * Determines whether loc1 or loc2 is closer to target without going beyond it.
 * Returns CLSR_NEITHER if both are beyond, CLSR_LOC1 if loc1 is closer, and
 * CLSR_LOC2 otherwise.
 */
inline CLOSENESS loc_closer(yyltype target, yyltype loc1, yyltype loc2) {
  int b1 = loc_before(loc1, target);
  int b2 = loc_before(loc2, target);

  /* make sure at least one is before target closer */
  if( !b1 && !b2 )
    return CLSR_NEITHER;

  /* if loc1 is beyond the target, then loc2 must be the closer one */
  if( !b1 )
    return CLSR_LOC2;

  /* if loc2 is beyond the target, then loc1 must be the closer one */
  if( !b2 )
    return CLSR_LOC1;

  /* both are before the line, so choose the one which is later (e.g. closer to the target) */
  if( loc_before(loc1, loc2) )
    return CLSR_LOC2;
  else
    return CLSR_LOC1;
}

#endif /* LOCATION_H */
