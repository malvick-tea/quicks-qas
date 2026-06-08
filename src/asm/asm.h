/*
 * qas — the assembler driver (public interface).
 *
 * Responsibility
 * Turn one assembly source into a complete ELF64 relocatable object. This is the
 * stage that ties the pipeline together: it parses the source, walks the resulting
 * statements assigning every label, directive, and instruction to an output
 * section, encodes the instructions, resolves intra-section references, turns the
 * rest into ELF relocations, and serializes the object image. It is the orchestrator
 * named in the encoder's header — the piece that "decides whether to resolve a
 * fixup locally or emit an ELF relocation."
 *
 * Pipeline (qas docs/design.md; Quicks-Meta roadmap Phase 2)
 *   source -> lexer -> parser -> [this driver: layout + encode + relocate] -> elf
 *
 * Separation of concerns
 *   The driver performs no I/O: it takes an already-loaded qas_source and returns
 *   the object image as a heap buffer. The CLI (app/main.c) owns reading the input
 *   file and writing the output, so the whole assembler remains unit-testable
 *   without touching the filesystem (coding-standard: no hidden side effects).
 *
 * Authorities
 *   - Intel SDM Vol 2 §2.1 (instruction encoding) — via the encoder module.
 *   - System V gABI (ELF) and the x86-64 psABI (EM_X86_64, RELA, R_X86_64_*) —
 *     via the elf module; the relocation choices are made here and cited at the
 *     point of decision in asm.c.
 */
#ifndef QAS_ASM_ASM_H
#define QAS_ASM_ASM_H

#include <stddef.h>
#include <stdint.h>

#include "diag/diag.h"
#include "source/source.h"
#include "status/status.h"

/*
 * Assemble `src` into an ELF64 relocatable object.
 *
 *   src        : the loaded source (borrowed; must outlive the call).
 *   diags      : diagnostics sink; lexical/syntax/semantic problems are reported
 *                here with source locations (borrowed; must outlive the call).
 *   out_image  : on full success, receives a heap buffer holding the object bytes
 *                (the caller frees it with free()); set to NULL if the source had
 *                any errors. Must be non-NULL.
 *   out_size   : receives the image length in bytes (0 when no image). Must be
 *                non-NULL.
 *
 * Contract (consistent with the rest of qas: status is for control flow,
 * diagnostics are for humans):
 *   - Returns QAS_OK whether or not the *source* was valid. Check *out_image: it
 *     is non-NULL exactly when assembly succeeded with no error diagnostics. When
 *     the source had errors, *out_image is NULL and the details are in `diags`.
 *   - Returns a non-OK status only for a *fatal* condition (QAS_ERR_OUT_OF_MEMORY,
 *     or QAS_ERR_INVALID_ARGUMENT for a NULL argument); no object is produced.
 */
qas_status qas_assemble(const qas_source *src, qas_diag_sink *diags,
                        uint8_t **out_image, size_t *out_size);

#endif /* QAS_ASM_ASM_H */
