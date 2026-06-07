/*
 * qas — ELF writer: serialization entry point (internal).
 *
 * Turns an accumulated builder into a complete ELF64 ET_REL image. Split out of
 * elf.c so the mutable builder and the one-shot serializer are separate
 * translation units (coding-standard §2). Used only by elf.c.
 */
#ifndef QAS_ELF_INTERNAL_SERIALIZE_H
#define QAS_ELF_INTERNAL_SERIALIZE_H

#include <stddef.h>
#include <stdint.h>

#include "elf/elf.h"

/*
 * Serialize *builder into a freshly malloc'd ELF64 object image. On success
 * *out_image owns the bytes (free with free()) and *out_size is its length; the
 * builder is not modified. On failure returns OOM/INVALID and sets *out_image to
 * NULL. See qas_elf_builder_finish for the public contract.
 */
qas_status qas_elf_serialize(const qas_elf_builder *builder, uint8_t **out_image,
                             size_t *out_size);

#endif /* QAS_ELF_INTERNAL_SERIALIZE_H */
