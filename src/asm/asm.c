/*
 * qas — the assembler driver (implementation).
 *
 * Three passes over one source:
 *
 *   1. Parse           source -> a list of statements (lexer + parser modules).
 *   2. Layout & encode walk the statements, switching the current output section
 *                      on directives, defining each label at the current offset,
 *                      encoding each instruction to bytes, and recording every
 *                      symbolic reference as a fixup in *section* coordinates.
 *                      Because every encoding in our subset is fixed-size (a
 *                      symbolic branch is always rel32, a symbolic memory or data
 *                      reference always 32/64-bit), instruction lengths do not
 *                      depend on label values, so a single forward pass places all
 *                      labels correctly and forward references are handled in
 *                      pass 3.
 *   3. Resolve & emit  for each fixup, either resolve it locally (a same-section
 *                      PC-relative reference to a *local* label, whose distance is
 *                      known now) by patching the bytes, or turn it into an ELF
 *                      relocation; then build and serialize the ELF object.
 *
 * Local resolution vs. relocation (the policy)
 *   A PC-relative reference whose target is a local label in the *same* section has
 *   a link-invariant relative distance, so we compute it and patch the rel32 in
 *   place — exactly what a conforming assembler does for local branches, and what
 *   lets the linker stay out of intra-function control flow. Everything else — any
 *   absolute reference (its value depends on where the linker places the section),
 *   any reference to a global symbol (it may be defined elsewhere or interposed),
 *   any cross-section reference, and any reference to an undefined symbol — becomes
 *   a relocation the linker completes (System V x86-64 psABI, R_X86_64_* table).
 */
#include "asm/asm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arena/arena.h"
#include "asm/internal/directive.h"
#include "asm/internal/unit.h"
#include "buf/buf.h"
#include "elf/elf.h"
#include "encoder/encoder.h"
#include "parser/parser.h"

/* A relocation the driver decided to emit, in terms it can finish later: the
   target output section, the site within it, the referencing symbol (by symtab
   index), the psABI relocation type, and the addend. */
typedef struct reloc_rec {
    uint32_t     out_section;
    uint64_t     offset;
    uint32_t     sym_index;
    Elf64_Word   type;
    Elf64_Sxword addend;
} reloc_rec;

static bool fits_int32(int64_t v)
{
    return v >= INT32_MIN && v <= INT32_MAX;
}

/*
 * Pass 2 of one instruction: encode it into the current section and record its
 * fixups in section coordinates. A failed encoding is a *user* error (the encoder
 * already emitted a diagnostic), so we report success-to-continue and let the
 * driver's final error check suppress the object. Only fatal statuses propagate.
 */
static qas_status emit_instruction(qas_asm_unit *unit, const qas_stmt *st)
{
    qas_asm_section *sec = qas_asm_unit_current(unit);
    if (sec != NULL && sec->type == SHT_NOBITS) {
        return qas_diag_emit(unit->diags, QAS_DIAG_ERROR, unit->src, st->offset,
                             st->length,
                             "cannot place an instruction in a NOBITS section (.bss)");
    }

    qas_encoded enc;
    qas_status st2 = qas_encode(unit->src, st, unit->diags, &enc);
    if (st2 != QAS_OK) {
        return (st2 == QAS_ERR_ENCODE) ? QAS_OK : st2; /* ENCODE = recoverable. */
    }

    /* The instruction begins at the current section offset; each fixup's field
       offset is that base plus the encoder's instruction-relative position. */
    uint64_t base = qas_asm_unit_here(unit);
    st2 = qas_asm_unit_emit_bytes(unit, enc.bytes, enc.len);
    if (st2 != QAS_OK) {
        return st2;
    }

    for (uint8_t k = 0; k < enc.fixup_count; ++k) {
        const qas_fixup *fx = &enc.fixups[k];
        qas_asm_fix_kind kind;
        switch (fx->kind) {
        case QAS_FIXUP_PC32:   kind = QAS_ASM_FIX_PC32;   break;
        case QAS_FIXUP_ABS32S: kind = QAS_ASM_FIX_ABS32S; break;
        case QAS_FIXUP_ABS64:  kind = QAS_ASM_FIX_ABS64;  break;
        default:               kind = QAS_ASM_FIX_ABS32S; break; /* unreachable */
        }
        st2 = qas_asm_unit_add_fix(unit, kind, fx->sym_off, fx->sym_len, fx->addend,
                                   base + fx->offset);
        if (st2 != QAS_OK) {
            return st2;
        }
    }
    return QAS_OK;
}

/* Map a driver fixup kind to its psABI relocation type. The computations (S = sym
   value, A = addend, P = place) are the psABI relocation-table definitions. */
static Elf64_Word reloc_type_of(qas_asm_fix_kind kind)
{
    switch (kind) {
    case QAS_ASM_FIX_PC32:   return R_X86_64_PC32; /* word32  S + A - P            */
    case QAS_ASM_FIX_ABS32S: return R_X86_64_32S;  /* word32  S + A  (sign-extended)*/
    case QAS_ASM_FIX_ABS32:  return R_X86_64_32;   /* word32  S + A  (zero-extended)*/
    case QAS_ASM_FIX_ABS64:  return R_X86_64_64;   /* word64  S + A                */
    }
    return R_X86_64_NONE;
}

/*
 * Pass 3a: walk the fixups, patching the ones we can resolve locally and recording
 * the rest as relocations. *out_recs is a freshly allocated array of rec_count
 * records (caller frees); it may be NULL when there are no relocations.
 */
static qas_status resolve_fixups(qas_asm_unit *unit, reloc_rec **out_recs,
                                 size_t *out_count)
{
    *out_recs  = NULL;
    *out_count = 0;

    size_t n = unit->fixes.count;
    reloc_rec *recs = NULL;
    if (n > 0) {
        recs = (reloc_rec *)malloc(n * sizeof(*recs));
        if (recs == NULL) {
            return QAS_ERR_OUT_OF_MEMORY;
        }
    }
    size_t rc = 0;

    for (size_t i = 0; i < n; ++i) {
        const qas_asm_fix *fx = &unit->fixes.items[i];
        uint32_t idx;
        qas_status st = qas_asm_symtab_intern(&unit->syms, unit->src->data + fx->sym_off,
                                              fx->sym_len, &idx);
        if (st != QAS_OK) {
            free(recs);
            return st;
        }
        qas_asm_sym *sym = qas_asm_symtab_at(&unit->syms, idx);

        bool local_pc32 = fx->kind == QAS_ASM_FIX_PC32 && sym->defined &&
                          sym->where == QAS_ASM_SYM_SECTION &&
                          sym->section == fx->out_section && !sym->is_global;
        if (local_pc32) {
            /* The rel32 the CPU adds to RIP. With the encoder's addend already
               carrying the -(4 + imm-tail), value = S + A - P with P = the field
               offset reduces to a section-local subtraction (the section base
               cancels because target and site share the section). */
            int64_t value = (int64_t)sym->value + fx->addend - (int64_t)fx->offset;
            if (!fits_int32(value)) {
                qas_status st2 = qas_diag_emit(unit->diags, QAS_DIAG_ERROR, unit->src,
                                               fx->sym_off, fx->sym_len,
                                               "PC-relative reference to '%.*s' is out "
                                               "of range (exceeds +/-2 GiB)",
                                               (int)fx->sym_len,
                                               unit->src->data + fx->sym_off);
                if (st2 != QAS_OK) {
                    free(recs);
                    return st2;
                }
                continue;
            }
            qas_asm_section *sec = qas_asm_section_set_at(&unit->sections, fx->out_section);
            qas_status st2 = qas_buf_patch_u32le(&sec->data, (size_t)fx->offset,
                                                 (uint32_t)value);
            if (st2 != QAS_OK) { /* would mean the site was never emitted: a bug. */
                free(recs);
                return st2;
            }
            continue;
        }

        /* Everything else is a relocation; force the symbol into .symtab. */
        sym->referenced = true;
        reloc_rec *r = &recs[rc++];
        r->out_section = fx->out_section;
        r->offset      = fx->offset;
        r->sym_index   = idx;
        r->type        = reloc_type_of(fx->kind);
        r->addend      = (Elf64_Sxword)fx->addend;
    }

    *out_recs  = recs;
    *out_count = rc;
    return QAS_OK;
}

/*
 * Pass 3b: build and serialize the ELF object. Only sections that carry content,
 * define an emitted symbol, or are a relocation target are included, so the object
 * is minimal. Symbols are emitted when they are global or referenced by a
 * relocation (defined-but-unreferenced locals are dropped, as a conforming
 * assembler does). The ELF builder synthesizes the .symtab, .strtab, per-section
 * .rela, and .shstrtab sections and orders locals before globals at finish.
 */
static qas_status build_object(qas_asm_unit *unit, const reloc_rec *recs,
                               size_t rec_count, uint8_t **out_image, size_t *out_size)
{
    qas_status      st = QAS_OK;
    size_t          S  = unit->sections.count;
    size_t          NS = unit->syms.count;
    qas_elf_builder b;
    qas_elf_builder_init(&b);

    uint32_t *sec_handle = (uint32_t *)malloc((S ? S : 1) * sizeof(uint32_t));
    bool     *include    = (bool *)calloc(S ? S : 1, sizeof(bool));
    if (sec_handle == NULL || include == NULL) {
        st = QAS_ERR_OUT_OF_MEMORY;
        goto done;
    }
    for (size_t i = 0; i < S; ++i) {
        sec_handle[i] = UINT32_MAX;
    }

    /* Decide which sections to materialize. A section is included if it holds any
       bytes/space, if an emitted symbol is defined in it, or if a relocation
       targets it. */
    for (size_t i = 0; i < S; ++i) {
        if (qas_asm_section_size(&unit->sections.items[i]) > 0) {
            include[i] = true;
        }
    }
    for (size_t i = 0; i < NS; ++i) {
        const qas_asm_sym *s = &unit->syms.syms[i];
        bool emit = s->is_global || s->referenced;
        if (emit && s->defined && s->where == QAS_ASM_SYM_SECTION) {
            include[s->section] = true;
        }
    }
    for (size_t k = 0; k < rec_count; ++k) {
        include[recs[k].out_section] = true;
    }

    /* Create the included sections in first-seen order and load their contents. */
    for (size_t i = 0; i < S; ++i) {
        if (!include[i]) {
            continue;
        }
        qas_asm_section *sec = &unit->sections.items[i];
        uint32_t handle;
        st = qas_elf_builder_add_section(&b, sec->name, sec->type, sec->flags,
                                         sec->addralign, &handle);
        if (st != QAS_OK) {
            goto done;
        }
        sec_handle[i] = handle;
        if (sec->type == SHT_NOBITS) {
            st = qas_elf_builder_reserve_bss(&b, handle, sec->bss_size, NULL);
        } else if (sec->data.len > 0) {
            st = qas_elf_builder_append(&b, handle, sec->data.data, sec->data.len, NULL);
        }
        if (st != QAS_OK) {
            goto done;
        }
    }

    /* Emit the symbol table entries. An undefined symbol must be global so the
       linker will resolve it (a local undefined symbol is meaningless). */
    for (size_t i = 0; i < NS; ++i) {
        qas_asm_sym *s = &unit->syms.syms[i];
        if (!(s->is_global || s->referenced)) {
            continue;
        }
        unsigned char bind = (s->is_global || !s->defined) ? STB_GLOBAL : STB_LOCAL;
        qas_elf_symref ref;
        uint32_t       section = 0;
        uint64_t       value   = 0;
        if (!s->defined) {
            ref = QAS_ELF_SYMREF_UNDEF;
        } else if (s->where == QAS_ASM_SYM_ABS) {
            ref   = QAS_ELF_SYMREF_ABS;
            value = s->value;
        } else {
            ref     = QAS_ELF_SYMREF_SECTION;
            section = sec_handle[s->section];
            value   = s->value;
        }
        uint32_t handle;
        st = qas_elf_builder_add_symbol(&b, s->name, bind, STT_NOTYPE, ref, section,
                                        value, 0, &handle);
        if (st != QAS_OK) {
            goto done;
        }
        s->emitted    = true;
        s->elf_handle = handle;
    }

    /* Emit relocations, now that both section and symbol handles exist. */
    for (size_t k = 0; k < rec_count; ++k) {
        const reloc_rec *r = &recs[k];
        qas_asm_sym     *s = &unit->syms.syms[r->sym_index];
        st = qas_elf_builder_add_rela(&b, sec_handle[r->out_section], r->offset,
                                      s->elf_handle, r->type, r->addend);
        if (st != QAS_OK) {
            goto done;
        }
    }

    st = qas_elf_builder_finish(&b, out_image, out_size);

done:
    free(sec_handle);
    free(include);
    qas_elf_builder_dispose(&b);
    return st;
}

qas_status qas_assemble(const qas_source *src, qas_diag_sink *diags,
                        uint8_t **out_image, size_t *out_size)
{
    if (src == NULL || diags == NULL || out_image == NULL || out_size == NULL) {
        return QAS_ERR_INVALID_ARGUMENT;
    }
    *out_image = NULL;
    *out_size  = 0;

    qas_arena     arena;
    qas_stmt_list list;
    qas_asm_unit  unit;
    reloc_rec    *recs        = NULL;
    size_t        rec_count   = 0;
    bool          unit_inited = false;

    qas_arena_init(&arena);
    qas_stmt_list_init(&list);

    /* Pass 1: parse. A non-OK status here is fatal (e.g. OOM); syntax errors are
       diagnostics and still return QAS_OK with a partial list. */
    qas_parser parser;
    qas_parser_init(&parser, src, diags, &arena);
    qas_status st = qas_parser_parse(&parser, &list);
    if (st != QAS_OK) {
        goto done;
    }

    st = qas_asm_unit_init(&unit, src, diags);
    if (st != QAS_OK) {
        goto done;
    }
    unit_inited = true;

    /* Pass 2: lay out and encode every statement. Handlers report user errors as
       diagnostics and return QAS_OK; only fatal statuses break the loop. */
    for (size_t i = 0; i < list.count; ++i) {
        const qas_stmt *s = &list.items[i];
        switch (s->kind) {
        case QAS_STMT_LABEL:
            st = qas_asm_unit_define_label(&unit, s->name_off, s->name_len);
            break;
        case QAS_STMT_DIRECTIVE:
            st = qas_asm_apply_directive(&unit, s);
            break;
        case QAS_STMT_INSTRUCTION:
            st = emit_instruction(&unit, s);
            break;
        default:
            st = QAS_OK;
            break;
        }
        if (st != QAS_OK) {
            goto done;
        }
    }

    /* Pass 3a: resolve fixups (patches local branches, collects relocations). */
    st = resolve_fixups(&unit, &recs, &rec_count);
    if (st != QAS_OK) {
        goto done;
    }

    /* Pass 3b: emit the object only if the source was clean. With any error
       diagnostic, *out_image stays NULL and the caller reports the diagnostics. */
    if (qas_diag_severity_count(diags, QAS_DIAG_ERROR) == 0) {
        st = build_object(&unit, recs, rec_count, out_image, out_size);
    }

done:
    free(recs);
    if (unit_inited) {
        qas_asm_unit_dispose(&unit);
    }
    qas_stmt_list_dispose(&list);
    qas_arena_dispose(&arena);
    return st;
}
