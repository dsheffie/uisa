// Runs a program-mode test natively (see host_prog.h) and prints its result
// the way prun does.
#include <stdio.h>
int prog_main(void);
int main(void) {
  printf("result %08x\n", (unsigned)prog_main());
  return 0;
}
