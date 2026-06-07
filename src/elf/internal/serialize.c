/*
 * qas — ELF writer: serialization pipeline.
 *
 * Produces an ET_REL image in the canonical order:
 *
 *   [0]            SHT_NULL header (all zero; required by the gABI)
 *   [1 .. S]       the caller's output sections, in the order added
 *   [S+1]          .symtab
 *   [S+2]          .strtab     (linked from .symtab via sh_link)
 *   [.. ]          .rela.<sec>  one per output section that has relocations
 *   [last]         .shstrtab   (named by e_shstrndx)
 *
 * The serializer never mutates the builder; everything computed at finish time
 * (final section indices, the local-before-global symbol order, string offsets)
 * lives in local arrays. Field values are cited to the System V gABI (generic
 * ELF) and the x86-64 psABI (EM_X86_64, RELA). All multi-byte output is little-
 * endian via the buf module (ELFDATA2LSB), so it is host-byte-order independent.
 */
#include "elf/internal/serialize.h"

#include <stdlib.h>
#include <string.h>

#include "buf/buf.h"
#include "elf/internal/model.h"

/* Propagate the first failing buffer op to the single cleanup path. */
#define CK(expr)                          \
    do {                                  \
        st = (expr);                      \
        if (st != QAS_OK) {               \
            goto done;                    \
        }                                 \
    } while (0)

/*
 * Pad `content` with zero bytes so that, placed `base` bytes into the file, its
 * end lands on an `align` boundary. Alignment is measured against the absolute
 * file offset (base + len), so it is correct for any alignment, not only ones
 * that happen to divide `base`. (gABI: a section's sh_offset must be congruent to
 * sh_addr modulo sh_addralign; for ET_REL sh_addr is 0, so file alignment is it.)
 */
static qas_status align_content(qas_buf *content, uint64_t base, uint64_t align)
{
    if (align <= 1u) {
        return QAS_OK;
    }
    uint64_t cur = base + (uint64_t)content->len;
    uint64_t rem = cur % align;
    return (rem == 0u) ? QAS_OK
                       : qas_buf_append_zeros(content, (size_t)(align - rem));
}

/* Serialize the 64-byte ELF header (System V gABI, "ELF Header"). */
static qas_status write_ehdr(qas_buf *img, Elf64_Half type, Elf64_Half machine,
                             Elf64_Off shoff, Elf64_Half shnum, Elf64_Half shstrndx)
{
    unsigned char ident[EI_NIDENT];
    memset(ident, 0, sizeof ident);
    ident[EI_MAG0] = ELFMAG0;
    ident[EI_MAG1] = ELFMAG1;
    ident[EI_MAG2] = ELFMAG2;
    ident[EI_MAG3] = ELFMAG3;
    ident[EI_CLASS]   = ELFCLASS64;    /* 64-bit object.                         */
    ident[EI_DATA]    = ELFDATA2LSB;   /* Little-endian (x86-64).                */
    ident[EI_VERSION] = EV_CURRENT;
    ident[EI_OSABI]   = ELFOSABI_SYSV;

    qas_status st;
    st = qas_buf_append(img, ident, EI_NIDENT);             if (st) return st;
    st = qas_buf_append_u16le(img, type);                   if (st) return st;
    st = qas_buf_append_u16le(img, machine);                if (st) return st;
    st = qas_buf_append_u32le(img, EV_CURRENT);             if (st) return st;
    st = qas_buf_append_u64le(img, 0);                      if (st) return st; /* e_entry  */
    st = qas_buf_append_u64le(img, 0);                      if (st) return st; /* e_phoff  */
    st = qas_buf_append_u64le(img, shoff);                  if (st) return st; /* e_shoff  */
    st = qas_buf_append_u32le(img, 0);                      if (st) return st; /* e_flags  */
    st = qas_buf_append_u16le(img, (uint16_t)sizeof(Elf64_Ehdr)); if (st) return st;
    st = qas_buf_append_u16le(img, 0);                      if (st) return st; /* e_phentsize */
    st = qas_buf_append_u16le(img, 0);                      if (st) return st; /* e_phnum  */
    st = qas_buf_append_u16le(img, (uint16_t)sizeof(Elf64_Shdr)); if (st) return st;
    st = qas_buf_append_u16le(img, shnum);                  if (st) return st;
    st = qas_buf_append_u16le(img, shstrndx);               if (st) return st;
    return QAS_OK;
}

/* Serialize one 64-byte section header in gABI field order. */
static qas_status write_shdr(qas_buf *img, const Elf64_Shdr *s)
{
    qas_status st;
    st = qas_buf_append_u32le(img, s->sh_name);      if (st) return st;
    st = qas_buf_append_u32le(img, s->sh_type);      if (st) return st;
    st = qas_buf_append_u64le(img, s->sh_flags);     if (st) return st;
    st = qas_buf_append_u64le(img, s->sh_addr);      if (st) return st;
    st = qas_buf_append_u64le(img, s->sh_offset);    if (st) return st;
    st = qas_buf_append_u64le(img, s->sh_size);      if (st) return st;
    st = qas_buf_append_u32le(img, s->sh_link);      if (st) return st;
    st = qas_buf_append_u32le(img, s->sh_info);      if (st) return st;
    st = qas_buf_append_u64le(img, s->sh_addralign); if (st) return st;
    st = qas_buf_append_u64le(img, s->sh_entsize);   if (st) return st;
    return QAS_OK;
}

qas_status qas_elf_serialize(const qas_elf_builder *b, uint8_t **out_image,
                             size_t *out_size)
{
    *out_image = NULL;
    *out_size  = 0;

    const size_t S  = b->section_count;
    const size_t NS = b->symbol_count;
    qas_status   st = QAS_OK;

    /* All resources NULL/empty up front so the single cleanup path is always
       safe, even if the very first allocation fails. */
    uint32_t   *rela_ndx  = NULL; /* out-section -> .rela header index (0 = none).*/
    uint32_t   *final_of  = NULL; /* builder symbol handle -> .symtab index.      */
    uint32_t   *order     = NULL; /* .symtab index (1..) -> builder symbol handle.*/
    uint32_t   *st_name   = NULL; /* .symtab index -> .strtab offset.             */
    uint32_t   *sh_name   = NULL; /* section header index -> .shstrtab offset.    */
    Elf64_Shdr *sh        = NULL; /* the section header table being built.        */
    qas_buf    *rela_bufs = NULL; /* per out-section serialized .rela bytes.      */
    qas_buf strtab, symtab, shstr, content, image;
    qas_buf_init(&strtab);
    qas_buf_init(&symtab);
    qas_buf_init(&shstr);
    qas_buf_init(&content);
    qas_buf_init(&image);

    /* 1. Assign section header indices in the canonical order described above. */
    rela_ndx = (uint32_t *)calloc(S ? S : 1, sizeof(uint32_t));
    if (rela_ndx == NULL) { st = QAS_ERR_OUT_OF_MEMORY; goto done; }

    const uint32_t symtab_ndx = (uint32_t)(1 + S);
    const uint32_t strtab_ndx = (uint32_t)(2 + S);
    uint32_t next = (uint32_t)(3 + S);
    for (size_t i = 0; i < S; ++i) {
        if (b->sections[i].reloc_count > 0) {
            rela_ndx[i] = next++;
        }
    }
    const uint32_t shstrtab_ndx = next++;
    const uint32_t shnum = next;

    /* 2. Symbol order: index 0 is the null symbol; then all locals (in insertion
       order), then all non-locals. The gABI requires every local symbol to
       precede the first global, and .symtab's sh_info to be that boundary. */
    final_of = (uint32_t *)malloc((NS ? NS : 1) * sizeof(uint32_t));
    order    = (uint32_t *)malloc((NS + 1) * sizeof(uint32_t));
    if (final_of == NULL || order == NULL) { st = QAS_ERR_OUT_OF_MEMORY; goto done; }
    order[0] = 0; /* placeholder for the synthetic null symbol */
    uint32_t pos = 1;
    for (size_t i = 0; i < NS; ++i) {
        if (b->symbols[i].bind == STB_LOCAL) {
            order[pos] = (uint32_t)i;
            final_of[i] = pos;
            pos++;
        }
    }
    const uint32_t first_global = pos;
    for (size_t i = 0; i < NS; ++i) {
        if (b->symbols[i].bind != STB_LOCAL) {
            order[pos] = (uint32_t)i;
            final_of[i] = pos;
            pos++;
        }
    }

    /* 3. .strtab: a leading NUL (so st_name 0 means "no name"), then each named
       symbol's name. Record every symtab entry's name offset. */
    st_name = (uint32_t *)calloc(NS + 1, sizeof(uint32_t));
    if (st_name == NULL) { st = QAS_ERR_OUT_OF_MEMORY; goto done; }
    CK(qas_buf_append_u8(&strtab, 0));
    st_name[0] = 0;
    for (uint32_t p = 1; p <= NS; ++p) {
        const qas_elf_symbol *sym = &b->symbols[order[p]];
        if (sym->name != NULL && sym->name[0] != '\0') {
            st_name[p] = (uint32_t)strtab.len;
            CK(qas_buf_append(&strtab, sym->name, strlen(sym->name) + 1));
        } else {
            st_name[p] = 0;
        }
    }

    /* 4. .symtab: a zero entry 0, then one Elf64_Sym per symbol in final order. */
    CK(qas_buf_append_zeros(&symtab, sizeof(Elf64_Sym)));
    for (uint32_t p = 1; p <= NS; ++p) {
        const qas_elf_symbol *sym = &b->symbols[order[p]];
        uint16_t shndx;
        switch (sym->ref) {
        case QAS_ELF_SYMREF_UNDEF:   shndx = SHN_UNDEF; break;
        case QAS_ELF_SYMREF_ABS:     shndx = SHN_ABS;   break;
        case QAS_ELF_SYMREF_SECTION: shndx = (uint16_t)(1u + sym->section); break;
        default: st = QAS_ERR_INVALID_ARGUMENT; goto done;
        }
        CK(qas_buf_append_u32le(&symtab, st_name[p]));
        CK(qas_buf_append_u8(&symtab, QAS_ELF64_ST_INFO(sym->bind, sym->type)));
        CK(qas_buf_append_u8(&symtab, 0)); /* st_other: default visibility */
        CK(qas_buf_append_u16le(&symtab, shndx));
        CK(qas_buf_append_u64le(&symtab, sym->value));
        CK(qas_buf_append_u64le(&symtab, sym->size));
    }

    /* 5. .rela.<sec> bytes for each section that has relocations. The symbol
       handle in each relocation is remapped to its final .symtab index. */
    rela_bufs = (qas_buf *)calloc(S ? S : 1, sizeof(qas_buf));
    if (rela_bufs == NULL) { st = QAS_ERR_OUT_OF_MEMORY; goto done; }
    for (size_t i = 0; i < S; ++i) {
        qas_buf_init(&rela_bufs[i]);
    }
    for (size_t i = 0; i < S; ++i) {
        const qas_elf_section *sec = &b->sections[i];
        for (size_t k = 0; k < sec->reloc_count; ++k) {
            const qas_elf_reloc *r = &sec->relocs[k];
            Elf64_Xword info = QAS_ELF64_R_INFO(final_of[r->symbol], r->type);
            CK(qas_buf_append_u64le(&rela_bufs[i], r->offset));
            CK(qas_buf_append_u64le(&rela_bufs[i], info));
            CK(qas_buf_append_i64le(&rela_bufs[i], r->addend));
        }
    }

    /* 6. .shstrtab: leading NUL, then every section's name; record sh_name for
       each header. A .rela section's name is ".rela" prepended to its target's
       name (e.g. ".text" -> ".rela.text"). */
    sh_name = (uint32_t *)calloc(shnum, sizeof(uint32_t));
    if (sh_name == NULL) { st = QAS_ERR_OUT_OF_MEMORY; goto done; }
    CK(qas_buf_append_u8(&shstr, 0));
    sh_name[0] = 0;
    for (size_t i = 0; i < S; ++i) {
        sh_name[1 + i] = (uint32_t)shstr.len;
        CK(qas_buf_append(&shstr, b->sections[i].name,
                          strlen(b->sections[i].name) + 1));
    }
    sh_name[symtab_ndx] = (uint32_t)shstr.len;
    CK(qas_buf_append(&shstr, ".symtab", 8));
    sh_name[strtab_ndx] = (uint32_t)shstr.len;
    CK(qas_buf_append(&shstr, ".strtab", 8));
    for (size_t i = 0; i < S; ++i) {
        if (rela_ndx[i] == 0) {
            continue;
        }
        sh_name[rela_ndx[i]] = (uint32_t)shstr.len;
        CK(qas_buf_append(&shstr, ".rela", 5)); /* no NUL: the name follows */
        CK(qas_buf_append(&shstr, b->sections[i].name,
                          strlen(b->sections[i].name) + 1));
    }
    sh_name[shstrtab_ndx] = (uint32_t)shstr.len;
    CK(qas_buf_append(&shstr, ".shstrtab", 10));

    /* 7. Lay out section contents right after the 64-byte ELF header, filling the
       section header table as we go. NOBITS occupies no file bytes. */
    sh = (Elf64_Shdr *)calloc(shnum, sizeof(Elf64_Shdr)); /* zeroed = NULL header */
    if (sh == NULL) { st = QAS_ERR_OUT_OF_MEMORY; goto done; }
    const uint64_t base = sizeof(Elf64_Ehdr);

    for (size_t i = 0; i < S; ++i) {
        const qas_elf_section *sec = &b->sections[i];
        uint32_t ndx = (uint32_t)(1 + i);
        sh[ndx].sh_name      = sh_name[ndx];
        sh[ndx].sh_type      = sec->type;
        sh[ndx].sh_flags     = sec->flags;
        sh[ndx].sh_addr      = 0;
        sh[ndx].sh_addralign = sec->addralign;
        sh[ndx].sh_entsize   = 0;
        sh[ndx].sh_link      = 0;
        sh[ndx].sh_info      = 0;
        if (sec->type == SHT_NOBITS) {
            sh[ndx].sh_offset = base + (uint64_t)content.len;
            sh[ndx].sh_size   = sec->bss_size;
        } else {
            CK(align_content(&content, base, sec->addralign));
            sh[ndx].sh_offset = base + (uint64_t)content.len;
            CK(qas_buf_append(&content, sec->data.data, sec->data.len));
            sh[ndx].sh_size   = sec->data.len;
        }
    }

    CK(align_content(&content, base, 8));
    sh[symtab_ndx].sh_name      = sh_name[symtab_ndx];
    sh[symtab_ndx].sh_type      = SHT_SYMTAB;
    sh[symtab_ndx].sh_offset    = base + (uint64_t)content.len;
    CK(qas_buf_append(&content, symtab.data, symtab.len));
    sh[symtab_ndx].sh_size      = symtab.len;
    sh[symtab_ndx].sh_link      = strtab_ndx;       /* .symtab links its strtab   */
    sh[symtab_ndx].sh_info      = first_global;      /* first non-local symbol     */
    sh[symtab_ndx].sh_addralign = 8;
    sh[symtab_ndx].sh_entsize   = sizeof(Elf64_Sym);

    sh[strtab_ndx].sh_name      = sh_name[strtab_ndx];
    sh[strtab_ndx].sh_type      = SHT_STRTAB;
    sh[strtab_ndx].sh_offset    = base + (uint64_t)content.len;
    CK(qas_buf_append(&content, strtab.data, strtab.len));
    sh[strtab_ndx].sh_size      = strtab.len;
    sh[strtab_ndx].sh_addralign = 1;

    for (size_t i = 0; i < S; ++i) {
        if (rela_ndx[i] == 0) {
            continue;
        }
        uint32_t ndx = rela_ndx[i];
        CK(align_content(&content, base, 8));
        sh[ndx].sh_name      = sh_name[ndx];
        sh[ndx].sh_type      = SHT_RELA;
        sh[ndx].sh_offset    = base + (uint64_t)content.len;
        CK(qas_buf_append(&content, rela_bufs[i].data, rela_bufs[i].len));
        sh[ndx].sh_size      = rela_bufs[i].len;
        sh[ndx].sh_link      = symtab_ndx;        /* relocations resolve via symtab */
        sh[ndx].sh_info      = (uint32_t)(1 + i);  /* the section they apply to      */
        sh[ndx].sh_addralign = 8;
        sh[ndx].sh_entsize   = sizeof(Elf64_Rela);
    }

    sh[shstrtab_ndx].sh_name      = sh_name[shstrtab_ndx];
    sh[shstrtab_ndx].sh_type      = SHT_STRTAB;
    sh[shstrtab_ndx].sh_offset    = base + (uint64_t)content.len;
    CK(qas_buf_append(&content, shstr.data, shstr.len));
    sh[shstrtab_ndx].sh_size      = shstr.len;
    sh[shstrtab_ndx].sh_addralign = 1;

    /* 8. The section header table follows the contents, 8-aligned. */
    CK(align_content(&content, base, 8));
    const uint64_t e_shoff = base + (uint64_t)content.len;

    /* 9. Assemble: header, then contents, then the section header table. */
    CK(write_ehdr(&image, ET_REL, EM_X86_64, e_shoff, (Elf64_Half)shnum,
                  (Elf64_Half)shstrtab_ndx));
    CK(qas_buf_append(&image, content.data, content.len));
    for (uint32_t n = 0; n < shnum; ++n) {
        CK(write_shdr(&image, &sh[n]));
    }

    /* 10. Hand the finished image to the caller; the builder is untouched. */
    CK(qas_buf_take(&image, out_image, out_size));

done:
    free(rela_ndx);
    free(final_of);
    free(order);
    free(st_name);
    free(sh_name);
    free(sh);
    if (rela_bufs != NULL) {
        for (size_t i = 0; i < S; ++i) {
            qas_buf_dispose(&rela_bufs[i]);
        }
        free(rela_bufs);
    }
    qas_buf_dispose(&strtab);
    qas_buf_dispose(&symtab);
    qas_buf_dispose(&shstr);
    qas_buf_dispose(&content);
    qas_buf_dispose(&image);
    return st;
}
