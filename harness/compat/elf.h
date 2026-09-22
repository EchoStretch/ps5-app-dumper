/* Just enough of <elf.h> for elf2fself.c on a machine that has none (macOS).
   The layout is the ELF64 standard; the PT_SCE_* values are the PS5 SDK's. */
#ifndef HARNESS_COMPAT_ELF_H
#define HARNESS_COMPAT_ELF_H
#include <stdint.h>

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef uint64_t Elf64_Xword;

typedef struct {
    unsigned char e_ident[16];
    Elf64_Half e_type, e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff, e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type, p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr, p_paddr;
    Elf64_Xword p_filesz, p_memsz, p_align;
} Elf64_Phdr;

#define PT_LOAD             1
#define PT_SCE_COMMENT      0x6FFFFF00
#define PT_SCE_DYNLIBDATA   0x61000000
#define PT_SCE_RELRO        0x61000010
#define PT_SCE_VERSION      0x6FFFFF01

#endif
