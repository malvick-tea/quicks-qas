/*
 * qas — command-line entry point.
 *
 * What works today
 *   The assembler is being built bottom-up (Quicks-Meta roadmap, Phase 2). The
 *   lexer is the first stage to land, so this CLI currently offers a
 *   `--dump-tokens` mode that scans a source file and prints its token stream
 *   plus any diagnostics. Full assembly (parse -> encode -> ELF) is not yet
 *   implemented; the CLI says so plainly rather than pretending.
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
#include <string.h>

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
            "qas — the Quicks assembler (early build)\n"
            "\n"
            "Usage:\n"
            "  %s --dump-tokens <file>   Scan <file> and print its token stream\n"
            "  %s --help                 Show this help\n"
            "  %s --version              Show version information\n"
            "\n"
            "Note: full assembly is not implemented yet; only --dump-tokens is\n"
            "available in this build.\n",
            prog, prog, prog);
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

int main(int argc, char **argv)
{
    const char *prog = (argc > 0 && argv[0] != NULL) ? argv[0] : "qas";

    const char *input_path = NULL;
    bool        dump_tokens = false;

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(stdout, prog);
            return QAS_EXIT_OK;
        }
        if (strcmp(arg, "--version") == 0) {
            /* Version is intentionally pre-1.0 during bootstrap. */
            printf("qas 0.0.1 (bootstrap; lexer only)\n");
            return QAS_EXIT_OK;
        }
        if (strcmp(arg, "--dump-tokens") == 0 || strcmp(arg, "-t") == 0) {
            dump_tokens = true;
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

    if (!dump_tokens) {
        /* The only implemented action so far. Be honest about it. */
        fprintf(stderr,
                "qas: assembling is not implemented yet; re-run with --dump-tokens\n");
        return QAS_EXIT_USAGE;
    }

    qas_source src;
    qas_status st = qas_source_load_file(input_path, &src);
    if (st != QAS_OK) {
        fprintf(stderr, "qas: cannot read '%s': %s\n", input_path, qas_status_str(st));
        return QAS_EXIT_ERROR;
    }

    int exit_code = run_dump_tokens(&src);
    qas_source_dispose(&src);
    return exit_code;
}
