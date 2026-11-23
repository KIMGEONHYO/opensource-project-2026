#define _POSIX_C_SOURCE 200809L
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_prompt(void)
{
    const char *prompt = getenv("MYSH_PROMPT");
    if (prompt == NULL || *prompt == '\0') {
        prompt = "mysh> ";
    }
    fputs(prompt, stdout);
    fflush(stdout);
}

static int read_command_line(char **line, size_t *capacity)
{
    ssize_t amount = getline(line, capacity, stdin);
    if (amount >= 0) {
        return 1;
    }
    if (feof(stdin)) {
        return 0;
    }
    perror("getline");
    clearerr(stdin);
    return -1;
}

static void report_parse_error(const char *line, const char *message)
{
    fprintf(stderr, "mysh: %s\n", message);
    if (line != NULL && *line != '\0') {
        fprintf(stderr, "  input: %s", line);
        size_t length = strlen(line);
        if (length == 0 || line[length - 1] != '\n') {
            fputc('\n', stderr);
        }
    }
}

int main(void)
{
    Shell shell = {0};
    shell.exit_status = 0;
    shell.last_status = 0;
    shell.next_job_id = 1;

    if (shell_init_terminal(&shell) == -1) {
        perror("shell initialization");
        return EXIT_FAILURE;
    }

    char *line = NULL;
    size_t capacity = 0;

    while (!shell.should_exit) {
        shell_reap_jobs(&shell);
        print_prompt();

        int read_result = read_command_line(&line, &capacity);
        if (read_result == 0) {
            putchar('\n');
            break;
        }
        if (read_result < 0) {
            continue;
        }

        ParsedLine parsed;
        char error[256] = {0};
        if (shell_parse(line, &parsed, error, sizeof(error)) == -1) {
            report_parse_error(line, error);
            shell.last_status = 2;
            continue;
        }

        if (parsed.pipeline.command_count == 0) {
            shell_free_parsed(&parsed);
            continue;
        }

        if (parsed.pipeline.background &&
            parsed.pipeline.command_count == 1 &&
            shell_is_parent_builtin(&parsed.pipeline.commands[0])) {
            fprintf(stderr,
                    "mysh: shell control commands cannot run in background\n");
            shell.last_status = 2;
            shell_free_parsed(&parsed);
            continue;
        }

        shell.last_status = shell_execute(&shell, &parsed.pipeline, line);
        shell_free_parsed(&parsed);
    }

    free(line);
    shell_restore_terminal(&shell);
    free(shell.old_directory);

    return shell.exit_status;
}
