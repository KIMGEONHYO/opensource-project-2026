#ifndef MINISHELL_SHELL_H
#define MINISHELL_SHELL_H

/*
 * Shared declarations for the teaching shell.
 *
 * The shell is divided into a parser, command implementations, and an
 * executor.  Keeping the common structures here makes the ownership rules
 * explicit and lets each part be reviewed independently.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define SHELL_MAX_LINE 4096
#define SHELL_MAX_TOKENS 256
#define SHELL_MAX_COMMANDS 16
#define SHELL_MAX_ARGS 64
#define SHELL_MAX_JOBS 32
#define SHELL_COMMAND_TEXT 256

typedef enum TokenKind {
    TOKEN_WORD,
    TOKEN_PIPE,
    TOKEN_INPUT,
    TOKEN_OUTPUT,
    TOKEN_APPEND,
    TOKEN_BACKGROUND
} TokenKind;

typedef struct Token {
    TokenKind kind;
    char *text;
} Token;

typedef struct Command {
    char *argv[SHELL_MAX_ARGS];
    size_t argc;
    char *input_path;
    char *output_path;
    bool append_output;
} Command;

typedef struct Pipeline {
    Command commands[SHELL_MAX_COMMANDS];
    size_t command_count;
    bool background;
} Pipeline;

typedef enum JobState {
    JOB_UNUSED,
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE
} JobState;

typedef struct Job {
    int id;
    pid_t process_group;
    pid_t processes[SHELL_MAX_COMMANDS];
    bool completed[SHELL_MAX_COMMANDS];
    bool stopped[SHELL_MAX_COMMANDS];
    size_t process_count;
    size_t completed_count;
    JobState state;
    char command[SHELL_COMMAND_TEXT];
} Job;

typedef struct Shell {
    bool interactive;
    bool should_exit;
    int exit_status;
    int last_status;
    pid_t process_group;
    int terminal_fd;
    char *old_directory;
    int next_job_id;
    Job jobs[SHELL_MAX_JOBS];
} Shell;

typedef struct ParsedLine {
    Token tokens[SHELL_MAX_TOKENS];
    size_t token_count;
    Pipeline pipeline;
} ParsedLine;

/* Parsing and token ownership. */
int shell_parse(const char *line, ParsedLine *parsed, char *error,
                size_t error_size);
void shell_free_parsed(ParsedLine *parsed);

/* Built-in commands implemented by this project. */
int shell_run_builtin(Shell *shell, const Command *command,
                      bool in_parent, bool *handled);
bool shell_is_parent_builtin(const Command *command);

/* Process and pipeline execution. */
int shell_execute(Shell *shell, const Pipeline *pipeline,
                  const char *source_line);
int shell_init_terminal(Shell *shell);
void shell_restore_terminal(Shell *shell);
void shell_reap_jobs(Shell *shell);
int shell_add_job(Shell *shell, pid_t pgid, const pid_t *pids,
                  size_t count, JobState state, const char *text);
Job *shell_find_job(Shell *shell, int id);
int shell_wait_for_job(Shell *shell, Job *job, bool foreground);
int shell_builtin_jobs(Shell *shell, const Command *command);
int shell_builtin_fg(Shell *shell, const Command *command);
int shell_builtin_bg(Shell *shell, const Command *command);

/* File command utilities. */
int command_ls(const Command *command);
int command_pwd(const Command *command);
int command_mkdir(const Command *command);
int command_rmdir(const Command *command);
int command_ln(const Command *command);
int command_cp(const Command *command);
int command_rm(const Command *command);
int command_mv(const Command *command);
int command_cat(const Command *command);
int command_grep(const Command *command);

#endif
