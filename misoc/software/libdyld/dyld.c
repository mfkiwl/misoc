#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dyld.h>

static int fixup_rela(struct dyld_info *info, const Elf32_Rela *rela,
                      Elf32_Addr (*resolve)(void *, const char *), void *resolve_data,
                      const char **error_out)
{
    const Elf32_Sym *sym = NULL;
    if(ELF32_R_SYM(rela->r_info) != 0)
        sym = &info->symtab[ELF32_R_SYM(rela->r_info)];
    Elf32_Addr value;

    switch(ELF32_R_TYPE(rela->r_info)) {
        case R_OR1K_NONE:
        return 1; // Does nothing.

        case R_OR1K_RELATIVE:
        value = info->base + rela->r_addend;
        break;

        case R_OR1K_32:
        case R_OR1K_GLOB_DAT:
        case R_OR1K_JMP_SLOT:
        value = (Elf32_Addr)dyld_lookup(&info->strtab[sym->st_name], info);
        if(value != 0)
            break;

        value = resolve(resolve_data, &info->strtab[sym->st_name]);
        if(value == 0) {
            static char error[256];
            snprintf(error, sizeof(error),
                     "ELF object has an unresolved symbol: %s",
                     &info->strtab[sym->st_name]);
            *error_out = error;
            return 0;
        }
        break;

        default:
        *error_out = "ELF object uses an unsupported relocation type";
        return 0;
    }

    memcpy((Elf32_Addr*)(info->base + rela->r_offset), &value,
           sizeof(Elf32_Addr));

    return 1;
}

int dyld_load(const void *shlib, Elf32_Addr base,
              Elf32_Addr (*resolve)(void *, const char *), void *resolve_data,
              struct dyld_info *info, const char **error_out)
{
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)shlib;

    const unsigned char expected_ident[EI_NIDENT] = {
        ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3,
        ELFCLASS32, ELFDATA2MSB, EV_CURRENT,
        ELFOSABI_NONE, /* ABI version */ 0
    };
    if(memcmp(ehdr->e_ident, expected_ident, EI_NIDENT) ||
       ehdr->e_type != ET_DYN) {
        *error_out = "ELF object is not a shared library";
        return 0;
    }

#ifdef __or1k__
    if(ehdr->e_machine != EM_OPENRISC) {
        *error_out = "ELF object does not contain OpenRISC machine code";
        return 0;
    }
#else
#error Unsupported architecture
#endif

    const Elf32_Phdr *phdr = (const Elf32_Phdr *)((intptr_t)shlib + ehdr->e_phoff);
    const Elf32_Dyn *dyn = NULL;
    for(int i = 0; i < ehdr->e_phnum; i++) {
        if(phdr[i].p_type == PT_DYNAMIC)
            dyn = (const Elf32_Dyn *)((intptr_t)shlib + phdr[i].p_offset);

        memcpy((void*)(base + phdr[i].p_vaddr),
               (const void*)((intptr_t)shlib + phdr[i].p_offset),
               phdr[i].p_filesz);
    }

    if(dyn == NULL) {
        *error_out = "ELF object does not have a PT_DYNAMIC header";
        return 0;
    }

    const char *strtab = NULL;
    const Elf32_Sym *symtab = NULL;
    const Elf32_Rela *rela = NULL, *pltrel = NULL;
    const Elf32_Word *hash = NULL;
    Elf32_Word init = 0;
    size_t syment = sizeof(Elf32_Sym), relaent = sizeof(Elf32_Rela),
           relanum = 0, pltrelnum = 0;
    while(dyn->d_tag != DT_NULL) {
        switch(dyn->d_tag) {
            case DT_STRTAB:   strtab    = (const char *)(base + dyn->d_un.d_ptr); break;
            case DT_SYMTAB:   symtab    = (const Elf32_Sym *)(base + dyn->d_un.d_ptr); break;
            case DT_SYMENT:   syment    = dyn->d_un.d_val; break;
            case DT_RELA:     rela      = (const Elf32_Rela *)(base + dyn->d_un.d_ptr); break;
            case DT_RELAENT:  relaent   = dyn->d_un.d_val; break;
            case DT_RELASZ:   relanum   = dyn->d_un.d_val / sizeof(Elf32_Rela); break;
            case DT_JMPREL:   pltrel    = (const Elf32_Rela *)(base + dyn->d_un.d_ptr); break;
            case DT_PLTRELSZ: pltrelnum = dyn->d_un.d_val / sizeof(Elf32_Rela); break;
            case DT_HASH:     hash      = (const Elf32_Word *)(base + dyn->d_un.d_ptr); break;

            case DT_REL:
            *error_out = "ELF object uses Rel relocations, which are not supported";
            return 0;
        }

        ++dyn;
    }

    if(symtab == NULL || syment == 0 || strtab == NULL) {
        *error_out = "ELF object must contain a symbol table";
        return 0;
    }

    if(syment != sizeof(Elf32_Sym) || relaent != sizeof(Elf32_Rela)) {
        *error_out = "ELF object uses an unknown format for symbols and relocations";
        return 0;
    }

    info->base         = base;
    info->strtab       = strtab;
    info->symtab       = symtab;
    info->hash.nbucket = hash[0];
    info->hash.nchain  = hash[1];
    info->hash.bucket  = &hash[2];
    info->hash.chain   = &hash[2 + info->hash.nbucket];

    for(int i = 0; i < relanum; i++) {
        if(!fixup_rela(info, &rela[i], resolve, resolve_data, error_out))
            return 0;
    }

    for(int i = 0; i < pltrelnum; i++) {
        if(!fixup_rela(info, &pltrel[i], resolve, resolve_data, error_out))
            return 0;
    }

    return 1;
}

static unsigned long elf_hash(const unsigned char *name)
{
    unsigned long h = 0, g;
    while(*name) {
        h = (h << 4) + *name++;
        if((g = h & 0xf0000000)) {
            h ^= g >> 24;
            h &= ~g;
        }
    }
    return h;
}

void *dyld_lookup(const char *symbol, struct dyld_info *info)
{
    unsigned hash = elf_hash((const unsigned char*) symbol);
    unsigned index = info->hash.bucket[hash % info->hash.nbucket];
    while(strcmp(&info->strtab[info->symtab[index].st_name], symbol)) {
        if(index == STN_UNDEF)
            return NULL;
        index = info->hash.chain[index];
    }

    Elf32_Addr value = info->symtab[index].st_value;
    if(value != 0)
        return (void*)(info->base + value);
    else
        return NULL;
}
