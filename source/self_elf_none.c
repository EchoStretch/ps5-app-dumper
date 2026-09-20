/* Stands in for the generated self_elf.c in the first build stage, and in
   any build that does not embed a copy of the ELF. */
#include <stddef.h>

const unsigned char self_elf[] = { 0 };
const size_t        self_elf_len = 0;
