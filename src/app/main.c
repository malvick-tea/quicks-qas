/*
 * qas — command-line entry point.
 *
 * What works today
 *   The assembler now runs the full pipeline (Quicks-Meta roadmap, Phase 2):
 *   it parses an Intel-syntax source, encodes the instructions, lays out the
 *   sections, resolves intra-section references, and writes an ELF64 relocatable
 *   object. The default action is to assemble; `--dump-tokens` remains available
 *   as a front-end diagnostic that prints the lexer's token stream.
 *
 * Exit codes (stable contract)
 *   0  success
 *   1  the input produced errors, or an I/O error occurred
 *   2  usage error (bad/missing arguments)
 *
 * This file is the only place that talks to argv and stdout/stderr for the tool;
 * the library modules (qas_core) stay free of I/O policy so they remain testable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm/asm.h"
#include "diag/diag.h"
#include "lexer/lexer.h"
#include "source/source.h"
#include "status/status.h"
#include "token/token.h"

/* Exit codes, named so call sites read clearly (no magic numbers). */
enum {
    QAS_EXIT_OK    = 0,
    QAS_EXIT_ERROR = 1,
    QAS_EXIT_USAGE = 2
};

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out,
            "qas — the Quicks assembler\n"
            "\n"
            "Usage:\n"
            "  %s [-o <out.o>] <in.s>     Assemble <in.s> to an ELF64 object\n"
            "  %s --dump-tokens <file>    Scan <file> and print its token stream\n"
            "  %s --help                  Show this help\n"
            "  %s --version               Show version information\n"
            "\n"
            "If -o is omitted, the object is written next to the input with a\n"
            "'.o' extension.\n",
            prog, prog, prog, prog);
}

/*
 * Print one token in a stable, greppable form:
 *   line:col  KIND  "lexeme"            (lexeme omitted for eof/newline)
 * Integer tokens also show their parsed value.
 */
static void print_token(const qas_source *src, const qas_token *tok)
{
    printf("%4u:%-3u  %-10s", (unsigned)tok->line, (unsigned)tok->column,
           qas_token_kind_name(tok->kind));

    switch (tok->kind) {
    case QAS_TOKEN_EOF:
        printf("  <eof>");
        break;
    case QAS_TOKEN_NEWLINE:
        printf("  \\n");
        break;
    case QAS_TOKEN_INTEGER:
        printf("  \"%.*s\"  = %llu%s", (int)tok->length, src->data + tok->offset,
               (unsigned long long)tok->int_value,
               tok->int_overflow ? "  (overflow)" : "");
        break;
    default:
        printf("  \"%.*s\"", (int)tok->length, src->data + tok->offset);
        break;
    }
    putchar('\n');
}

/*
 * Run the token-dump action on an already-loaded source. Returns a process exit
 * code. Diagnostics are printed after the token list so the stream is readable.
 */
static int run_dump_tokens(const qas_source *src)
{
    qas_diag_sink diags;
    qas_diag_sink_init(&diags);

    qas_lexer lexer;
    qas_lexer_init(&lexer, src, &diags);

    int exit_code = QAS_EXIT_OK;
    for (;;) {
        qas_token tok;
        qas_status st = qas_lexer_next(&lexer, &tok);
        if (st != QAS_OK) {
            /* The only non-OK status from the lexer is a catastrophic one
               (e.g. out of memory while recording a diagnostic). */
            fprintf(stderr, "qas: internal error: %s\n", qas_status_str(st));
            exit_code = QAS_EXIT_ERROR;
            break;
        }
        print_token(src, &tok);
        if (tok.kind == QAS_TOKEN_EOF) {
            break;
        }
    }

    if (qas_diag_count(&diags) > 0) {
        fputc('\n', stderr);
        qas_diag_sink_print(&diags, stderr);
    }
    if (qas_diag_severity_count(&diags, QAS_DIAG_ERROR) > 0) {
        exit_code = QAS_EXIT_ERROR;
    }

    qas_diag_sink_dispose(&diags);
    return exit_code;
}

/*
 * Derive a default output path from the input: replace the final extension with
 * ".o", or append ".o" if the base name has none. Returns a malloc'd string the
 * caller frees, or NULL on allocation failure.
 */
static char *default_output_path(const char *input)
{
    size_t len = strlen(input);

    /* Find the last extension dot, but only within the final path component, so a
       directory like "../a.b/in" is not mistaken for an extension. */
    size_t base = 0;
    for (size_t i = 0; i < len; ++i) {
        if (input[i] == '/' || input[i] == '\\') {
            base = i + 1;
        }
    }
    size_t dot = len; /* "no dot" sentinel. */
    for (size_t i = base; i < len; ++i) {
        if (input[i] == '.') {
            dot = i;
        }
    }

    char *out;
    if (dot == len) {
        out = (char *)malloc(len + 3); /* + ".o" + NUL */
        if (out != NULL) {
            memcpy(out, input, len);
            memcpy(out + len, ".o", 3);
        }
    } else {
        out = (char *)malloc(dot + 3); /* keep up to the dot, + "o" + NUL */
        if (out != NULL) {
            memcpy(out, input, dot + 1); /* include the '.' */
            out[dot + 1] = 'o';
            out[dot + 2] = '\0';
        }
    }
    return out;
}

/* Write `size` bytes to `path` in binary mode. Returns QAS_OK or QAS_ERR_IO. */
static qas_status write_file(const char *path, const uint8_t *bytes, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return QAS_ERR_IO;
    }
    size_t written = (size > 0) ? fwrite(bytes, 1, size, f) : 0;
    int    closed  = fclose(f);
    if (written != size || closed != 0) {
        return QAS_ERR_IO;
    }
    return QAS_OK;
}

/*
 * Assemble `src` and, on success, write the object to `output_path`. Returns a
 * process exit code; diagnostics are printed to stderr.
 */
static int run_assemble(const qas_source *src, const char *output_path)
{
    qas_diag_sink diags;
    qas_diag_sink_init(&diags);

    uint8_t *image = NULL;
    size_t   size  = 0;
    qas_status st = qas_assemble(src, &diags, &image, &size);

    int exit_code = QAS_EXIT_OK;
    if (st != QAS_OK) {
        /* Fatal (out of memory, bad argument): not a normal user error. */
        fprintf(stderr, "qas: internal error: %s\n", qas_status_str(st));
        exit_code = QAS_EXIT_ERROR;
    } else if (image == NULL) {
        /* The source had errors; the diagnostics below explain them. */
        exit_code = QAS_EXIT_ERROR;
    } else {
        qas_status wst = write_file(output_path, image, size);
        if (wst != QAS_OK) {
            fprintf(stderr, "qas: cannot write '%s': %s\n", output_path,
                    qas_status_str(wst));
            exit_code = QAS_EXIT_ERROR;
        }
    }

    if (qas_diag_count(&diags) > 0) {
        qas_diag_sink_print(&diags, stderr);
    }

    free(image);
    qas_diag_sink_dispose(&diags);
    return exit_code;
}

int main(int argc, char **argv)
{
    const char *prog = (argc > 0 && argv[0] != NULL) ? argv[0] : "qas";

    const char *input_path  = NULL;
    const char *output_path = NULL;
    bool        dump_tokens = false;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(stdout, prog);
            return QAS_EXIT_OK;
        }
        if (strcmp(arg, "--version") == 0) {
            /* Version is intentionally pre-1.0 during bootstrap. */
            printf("qas 0.1.0 (bootstrap)\n");
            return QAS_EXIT_OK;
        }
        if (strcmp(arg, "--dump-tokens") == 0 || strcmp(arg, "-t") == 0) {
            dump_tokens = true;
            continue;
        }
        if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "qas: option '%s' needs an argument\n", arg);
                return QAS_EXIT_USAGE;
            }
            output_path = argv[++i];
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "qas: unknown option '%s'\n", arg);
            print_usage(stderr, prog);
            return QAS_EXIT_USAGE;
        }
        if (input_path != NULL) {
            fprintf(stderr, "qas: more than one input file given ('%s' and '%s')\n",
                    input_path, arg);
            return QAS_EXIT_USAGE;
        }
        input_path = arg;
    }

    if (input_path == NULL) {
        fprintf(stderr, "qas: no input file\n");
        print_usage(stderr, prog);
        return QAS_EXIT_USAGE;
    }

    qas_source src;
    qas_status st = qas_source_load_file(input_path, &src);
    if (st != QAS_OK) {
        fprintf(stderr, "qas: cannot read '%s': %s\n", input_path, qas_status_str(st));
        return QAS_EXIT_ERROR;
    }

    int exit_code;
    if (dump_tokens) {
        exit_code = run_dump_tokens(&src);
    } else {
        /* Assemble. Use an explicit -o, or a name derived from the input. */
        char *derived = NULL;
        const char *out = output_path;
        if (out == NULL) {
            derived = default_output_path(input_path);
            if (derived == NULL) {
                fprintf(stderr, "qas: out of memory\n");
                qas_source_dispose(&src);
                return QAS_EXIT_ERROR;
            }
            out = derived;
        }
        exit_code = run_assemble(&src, out);
        free(derived);
    }

    qas_source_dispose(&src);
    return exit_code;
}
