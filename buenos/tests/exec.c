/*
 * Userland exec test
 */

#include "tests/lib.h"

static const char prog[] = "[disk1]hw"; /* The program to start. */

int main(void)
{
  uint32_t child;
  int ret;
  printf("Starting program %s\n", prog);
  child = syscall_exec(prog);
  printf("Now joining child %d\n", child);
  ret = (char)syscall_join(child);
  printf("Child joined with status: %d\n", ret);
  syscall_halt();
  return 0;
}
