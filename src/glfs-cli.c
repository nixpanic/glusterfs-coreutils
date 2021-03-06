/**
 * Copyright (C) 2015 Facebook Inc.
 *
 * Entry point of all utilities except for put. Also acts as an interactive
 * shell when invoked directly.
 */

#include <config.h>

#include <ctype.h>
#include <errno.h>
#include <error.h>
#include <getopt.h>
#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glfs-cli.h"
#include "glfs-cat.h"
#include "glfs-cp.h"
#include "glfs-cli-commands.h"
#include "glfs-ls.h"
#include "glfs-mkdir.h"
#include "glfs-rm.h"
#include "glfs-stat.h"
#include "glfs-tail.h"
#include "glfs-util.h"

#define AUTHORS "Written by Craig Cabrey."

extern bool debug;

struct cli_context *ctx;

struct cmd {
        char *alias;
        char *name;
        int (*execute) (struct cli_context *ctx);
};

static int
shell_usage ()
{
        printf ("The following commands are supported:\n"
                "* cat\n"
                "* connect\n"
                "* cp\n"
                "* disconnect\n"
                "* help\n"
                "* ls\n"
                "* mkdir\n"
                "* quit\n"
                "* rm\n"
                "* stat\n"
                "* tail\n");

        return 0;
}

#define NUM_CMDS 12
static struct cmd const cmds[] =
{
        { .name = "connect", .execute = cli_connect },
        { .name = "disconnect", .execute = cli_disconnect },
        { .alias = "gfcat", .name = "cat", .execute = do_cat },
        { .alias = "gfcp", .name = "cp", .execute = do_cp },
        { .name = "help", .execute = shell_usage },
        { .alias = "gfls", .name = "ls", .execute = do_ls },
        { .alias = "gfmkdir", .name = "mkdir", .execute = do_mkdir },
        { .alias = "gfmv", .name = "mv", .execute = not_implemented },
        { .name = "quit", .execute = handle_quit },
        { .alias = "gfrm", .name = "rm", .execute = do_rm },
        { .alias = "gfstat", .name = "stat", .execute = do_stat },
        { .alias = "gftail", .name = "tail", .execute = do_tail }
};

static const struct cmd*
get_cmd (char *name)
{
        const struct cmd *cmd = NULL;
        for (int j = 0; j < NUM_CMDS; j++) {
                if (strcmp (name, cmds[j].name) == 0 ||
                                (cmds[j].alias != NULL &&
                                strcmp (name, cmds[j].alias) == 0)) {
                        cmd = &(cmds[j]);
                        break;
                }
        }

        return cmd;
}

/**
 * Trim white space characters from the end of a null terminated string.
 */
static void
trim (char *str)
{
        size_t len = strlen (str);
        for (size_t i = len - 1; i >= 0; i--) {
                if (isspace (str[i])) {
                        str[i] = '\0';
                } else {
                        break;
                }
        }
}

static int
split_str (char *line, char **argv[])
{
        int argc = 0;
        char *line_start;

        line_start = line;

        while (*line != '\0') {
                if (*line == ' ') {
                        argc++;
                }

                line++;
        }

        argc++;
        line = line_start;

        *argv = malloc (sizeof (char*) * argc);
        if (*argv == NULL) {
                goto out;
        }

        int cur_arg = 0;
        while (cur_arg < argc && *line != '\0') {
                (*argv)[cur_arg] = line;

                while (*line != ' ' && *line != '\n' && *line != '\0') {
                        line++;
                }

                *line = '\0';
                line++;
                cur_arg++;
        }

out:
        return argc;
}

static int
start_shell ()
{
        int ret = 0;
        char *line = NULL;
        char *pos = NULL;
        char *token;
        size_t size;
        const struct cmd *cmd;

        while (true) {
                // getline () indicates that passing a NULL char* will cause
                // an allocation on the caller's behalf. Therefore, we need to
                // free line and reset it to NULL every time before calling
                // getline ().
                free (line);
                line = NULL;

                if (ctx->conn_str) {
                        printf ("gfcli %s> ", ctx->conn_str);
                } else {
                        printf ("gfcli> ");
                }

                ret = 0;
                if (getline (&line, &size, stdin) == -1) {
                        ret = -1;
                        goto out;
                }

                if (*line == '\n') {
                        continue;
                }

                char *line_copy = strdup (line);
                if (line_copy == NULL) {
                        error (0, errno, "strdup");
                        goto out;
                }

                token = strtok_r (line_copy, " ", &pos);
                trim (token);
                cmd = get_cmd (token);

                if (cmd) {
                        ctx->argc = split_str (line, &ctx->argv);
                        if (ctx->argc == 0) {
                                goto out;
                        }

                        program_invocation_name = ctx->argv[0];
                        ret = cmd->execute (ctx);
                        free (ctx->argv);
                        ctx->argv = NULL;
                } else {
                        fprintf (stderr,
                                "Unknown command '%s'. Type 'help' for more.\n",
                                token);
                }

                free (line_copy);
        }

out:
        free (line);
        return ret;
}

static struct option const long_options[] =
{
        {"debug", no_argument, NULL, 'd'},
        {"help", no_argument, NULL, 'x'},
        {"version", no_argument, NULL, 'v'},
        {"xlator-option", required_argument, NULL, 'o'},
        {NULL, no_argument, NULL, 0}
};

static void
usage ()
{
        printf ("Usage: %s [OPTION]... [URL]\n"
                "Start a Gluster shell to execute commands on a remote Gluster volume.\n\n"
                "  -o, --xlator-option=OPTION   specify a translator option for the\n"
                "                               connection. Multiple options are supported\n"
                "                               and take the form xlator.key=value.\n"
                "  -p, --port=PORT              specify a port on which to connect\n"
                "      --help     display this help and exit\n"
                "      --version  output version information and exit\n\n"
                "Examples:\n"
                "  gfcli glfs://localhost/groot\n"
                "        Start a shell with a connection to localhost opened.\n"
                "  gfcli -o *replicate*.data-self-heal=on glfs://localhost/groot\n"
                "        Start a shell with a connection localhost open, with the\n"
                "        translator option data-self-head set to on.\n",
                program_invocation_name);
        exit (EXIT_SUCCESS);
}

static void
parse_options (struct cli_context *ctx)
{
        int argc = ctx->argc;
        char **argv = ctx->argv;
        int opt = 0;
        int option_index = 0;
        struct xlator_option *option;

        while (true) {
                opt = getopt_long (argc, argv, "o:", long_options,
                                &option_index);

                if (opt == -1) {
                        break;
                }

                switch (opt) {
                        case 'd':
                                ctx->options->debug = true;
                                break;
                        case 'o':
                                option = parse_xlator_option (optarg);
                                if (option == NULL) {
                                        error (EXIT_FAILURE, errno, "%s", optarg);
                                }

                                if (append_xlator_option (&ctx->options->xlator_options, option) == -1) {
                                        error (EXIT_FAILURE, errno, "append_xlator_option");
                                }

                                break;
                        case 'h':
                                usage ();
                                exit (EXIT_SUCCESS);
                        case 'v':
                                printf ("%s (%s) %s\n%s\n%s\n%s\n",
                                                program_invocation_name,
                                                PACKAGE_NAME,
                                                PACKAGE_VERSION,
                                                COPYRIGHT,
                                                LICENSE,
                                                AUTHORS);
                                exit (EXIT_SUCCESS);
                        case 'x':
                                usage ();
                        default:
                                error (EXIT_FAILURE, 0, "Try --help for more information.");
                }
        }

        if (argc - option_index >= 2) {
                if (cli_connect (ctx) == -1) {
                        exit (EXIT_FAILURE);
                }

                if (apply_xlator_options (ctx->fs, &ctx->options->xlator_options) == -1) {
                        exit (EXIT_FAILURE);
                }
        }
}

void
cleanup ()
{
        if (ctx->url) {
                gluster_url_free (ctx->url);
        }

        if (ctx->options) {
                free_xlator_options (&ctx->options->xlator_options);
                free (ctx->options);
        }

        if (ctx->fs) {
                glfs_fini (ctx->fs);
        }

        free (ctx);
}

void
sig_handler (int sig)
{
        if (sig == SIGINT) {
                cleanup ();
                exit (EXIT_SUCCESS);
        }
}

int
main (int argc, char *argv[])
{
        int ret = 0;
        const struct cmd *cmd = NULL;

        // We need to catch SIGINT so that we can gracefully close the
        // connection to the Gluster node(s); this prevents potential issues
        // with buffers not being fully flushed.
        signal (SIGINT, sig_handler);
        program_invocation_name = basename (argv[0]);
        atexit (close_stdout);

        ctx = malloc (sizeof (*ctx));
        if (ctx == NULL) {
                error (EXIT_FAILURE, errno, "failed to initialize context");
        }

        ctx->argc = argc;
        ctx->argv = argv;
        ctx->conn_str = NULL;
        ctx->fs = NULL;
        ctx->options = malloc (sizeof (*(ctx->options)));
        if (ctx->options == NULL) {
                error (EXIT_FAILURE, errno, "failed to initialize options");
        }

        ctx->options->debug = false;
        ctx->options->xlator_options = NULL;
        ctx->url = NULL;

        cmd = get_cmd (program_invocation_name);

        if (cmd) {
                ctx->in_shell = false;
                ret = cmd->execute (ctx);
        } else {
                // Only parse options if we are being invoked as a shell
                ctx->in_shell = true;
                parse_options (ctx);

                // Set ctx->argv to NULL in the case that we enter the shell
                // and immediately receive a SIGINT. Without this, we would be
                // trying to free () the cli's argv, which would result in an
                // invalid free ().
                ctx->argc = 0;
                ctx->argv = NULL;
                ret = start_shell ();

                if (ctx->options->debug) {
                        print_xlator_options (&ctx->options->xlator_options);
                }

                ret = start_shell ();
        }

        cleanup ();

        if (ret == -1) {
                ret = EXIT_FAILURE;
        }

        return ret;
}
