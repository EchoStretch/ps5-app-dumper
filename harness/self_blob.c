/* Stands in for the payload's embedded copy of itself, so "Keep this version
   in Payload Manager" has something to upload and to compare with. */
#include <stddef.h>

const unsigned char self_elf[] = "\177ELF-this-stands-in-for-the-payload";
const size_t        self_elf_len = sizeof(self_elf) - 1;
