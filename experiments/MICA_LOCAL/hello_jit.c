/*
 * Simple hello program for testing 
 */

#include <stdint.h>

static uint64_t(*print64)(const char*, ...) = (void *)2;

// This hack is needed because the ubpf ELF loader doesn't support
// rodata sections to hold string literals. This trick forces those
// strings into the code at the cost of some extra runtime overhead and
// binary bloat.
#define printm(fmt, ...) \
  ({ \
      char ____fmt[] = fmt; \
      print64(____fmt, ##__VA_ARGS__); \
  })

uint64_t prog(void *pkt) {
  printm("Hello, JIT\n");
  return 0;
}
