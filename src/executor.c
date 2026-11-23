#define _POSIX_C_SOURCE 200809L
#include "shell.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t child_event_received;

static void note_child_event(int signal_number)
{
    (void)signal_number;
    child_event_received = 1;
}

static int change_signal(int signal_number, void (*handler)(int))
{
    struct sigaction action = {0};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    return sigaction(signal_number, &action, NULL);
}

static int ignore_signal(int signal_number)
{
    return change_signal(signal_number, SIG_IGN);
}

static void restore_child_signal_defaults(void)
{
    (void)change_signal(SIGINT, SIG_DFL);
    (void)change_signal(SIGQUIT, SIG_DFL);
    (void)change_signal(SIGTSTP, SIG_DFL);
    (void)change_signal(SIGTTIN, SIG_DFL);
    (void)change_signal(SIGTTOU, SIG_DFL);
    (void)change_signal(SIGCHLD, SIG_DFL);
}

int shell_init_terminal(Shell *shell)
{
    if (shell == NULL) {
        errno = EINVAL;
        return -1;
    }

    shell->terminal_fd = STDIN_FILENO;
    shell->interactive = isatty(shell->terminal_fd) != 0;
    shell->process_group = getpgrp();

    if (shell->interactive) {
        pid_t current_group = getpgrp();
        while (tcgetpgrp(shell->terminal_fd) != current_group) {
            (void)kill(-current_group, SIGTTIN);
            current_group = getpgrp();
        }

        pid_t own_group = getpid();
        if (setpgid(own_group, own_group) == -1 && errno != EACCES &&
            errno != EPERM) {
            return -1;
        }
        shell->process_group = getpgrp();
        if (tcsetpgrp(shell->terminal_fd, shell->process_group) == -1) {
            return -1;
        }
    }

    if (ignore_signal(SIGINT) == -1 || ignore_signal(SIGQUIT) == -1 ||
        ignore_signal(SIGTSTP) == -1 || ignore_signal(SIGTTIN) == -1 ||
        ignore_signal(SIGTTOU) == -1 ||
        change_signal(SIGCHLD, note_child_event) == -1) {
        return -1;
    }
    return 0;
}

void shell_restore_terminal(Shell *shell)
{
    if (shell != NULL && shell->interactive) {
        (void)tcsetpgrp(shell->terminal_fd, shell->process_group);
    }
}

static void mark_process_status(Job *job, pid_t process, int status)
{
    for (size_t index = 0; index < job->process_count; index++) {
        if (job->processes[index] != process) {
            continue;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (!job->completed[index]) {
                job->completed[index] = true;
                job->completed_count++;
            }
            job->stopped[index] = false;
        } else if (WIFSTOPPED(status)) {
            job->stopped[index] = true;
            job->state = JOB_STOPPED;
        } else if (WIFCONTINUED(status)) {
            job->stopped[index] = false;
            job->state = JOB_RUNNING;
        }
        return;
    }
}

static bool job_is_complete(const Job *job)
{
    return job->process_count > 0 &&
           job->completed_count >= job->process_count;
}

static bool job_is_fully_stopped(const Job *job)
{
    bool has_live_process = false;
    for (size_t index = 0; index < job->process_count; index++) {
        if (job->completed[index]) {
            continue;
        }
        has_live_process = true;
        if (!job->stopped[index]) {
            return false;
        }
    }
    return has_live_process;
}

void shell_reap_jobs(Shell *shell)
{
    if (shell == NULL) {
        return;
    }

    if (!child_event_received) {
        return;
    }
    child_event_received = 0;

    for (size_t job_index = 0; job_index < SHELL_MAX_JOBS; job_index++) {
        Job *job = &shell->jobs[job_index];
        if (job->state == JOB_UNUSED || job->state == JOB_DONE) {
            continue;
        }

        for (size_t process_index = 0;
             process_index < job->process_count; process_index++) {
            if (job->completed[process_index]) {
                continue;
            }

            int status = 0;
            pid_t result = waitpid(job->processes[process_index], &status,
                                   WNOHANG | WUNTRACED | WCONTINUED);
            if (result > 0) {
                mark_process_status(job, result, status);
            } else if (result == -1 && errno == ECHILD) {
                job->completed[process_index] = true;
                job->completed_count++;
            }
        }

        if (job_is_complete(job)) {
            job->state = JOB_DONE;
        }
    }
}

static Job *allocate_job(Shell *shell)
{
    for (size_t index = 0; index < SHELL_MAX_JOBS; index++) {
        if (shell->jobs[index].state == JOB_UNUSED ||
            shell->jobs[index].state == JOB_DONE) {
            memset(&shell->jobs[index], 0, sizeof(shell->jobs[index]));
            shell->jobs[index].state = JOB_RUNNING;
            shell->jobs[index].id = shell->next_job_id++;
            if (shell->next_job_id <= 0) {
                shell->next_job_id = 1;
            }
            return &shell->jobs[index];
        }
    }
    return NULL;
}

int shell_add_job(Shell *shell, pid_t pgid, const pid_t *pids,
                  size_t count, JobState state, const char *text)
{
    if (shell == NULL || pids == NULL || count == 0 ||
        count > SHELL_MAX_COMMANDS) {
        errno = EINVAL;
        return -1;
    }

    Job *job = allocate_job(shell);
    if (job == NULL) {
        errno = ENOSPC;
        return -1;
    }

    job->process_group = pgid;
    job->process_count = count;
    job->state = state;
    memcpy(job->processes, pids, count * sizeof(*pids));

    if (text != NULL) {
        (void)snprintf(job->command, sizeof(job->command), "%s", text);
    }
    return job->id;
}

Job *shell_find_job(Shell *shell, int id)
{
    if (shell == NULL) {
        return NULL;
    }

    for (size_t index = 0; index < SHELL_MAX_JOBS; index++) {
        Job *job = &shell->jobs[index];
        if (job->state != JOB_UNUSED && job->id == id) {
            return job;
        }
    }
    return NULL;
}

static int wait_status_for_job(Job *job, pid_t pid, int status)
{
    mark_process_status(job, pid, status);
    if (job_is_complete(job)) {
        job->state = JOB_DONE;
    }
    return 0;
}

int shell_wait_for_job(Shell *shell, Job *job, bool foreground)
{
    if (shell == NULL || job == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (foreground && shell->interactive &&
        tcsetpgrp(shell->terminal_fd, job->process_group) == -1) {
        perror("tcsetpgrp");
    }

    job->state = JOB_RUNNING;
    while (!job_is_complete(job)) {
        int status = 0;
        pid_t result = waitpid(-job->process_group, &status,
                               WUNTRACED | WCONTINUED);
        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                job->completed_count = job->process_count;
                job->state = JOB_DONE;
                break;
            }
            perror("waitpid");
            shell_restore_terminal(shell);
            return -1;
        }

        wait_status_for_job(job, result, status);
        if (WIFSTOPPED(status) && job_is_fully_stopped(job)) {
            job->state = JOB_STOPPED;
            break;
        }
    }

    if (foreground) {
        shell_restore_terminal(shell);
    }

    return 0;
}

static const char *job_state_name(JobState state)
{
    switch (state) {
    case JOB_RUNNING:
        return "Running";
    case JOB_STOPPED:
        return "Stopped";
    case JOB_DONE:
        return "Done";
    case JOB_UNUSED:
    default:
        return "Unused";
    }
}

int shell_builtin_jobs(Shell *shell, const Command *command)
{
    (void)command;
    shell_reap_jobs(shell);
    bool found = false;

    for (size_t index = 0; index < SHELL_MAX_JOBS; index++) {
        Job *job = &shell->jobs[index];
        if (job->state == JOB_UNUSED) {
            continue;
        }
        found = true;
        printf("[%d] %-7s %s\n", job->id, job_state_name(job->state),
               job->command);
    }

    if (!found) {
        puts("No jobs");
    }
    return 0;
}

static int parse_job_id(const Command *command, int *job_id)
{
    if (command->argc == 1) {
        *job_id = -1;
        return 0;
    }
    if (command->argc != 2) {
        fprintf(stderr, "%s: expected zero or one job number\n",
                command->argv[0]);
        return -1;
    }

    const char *text = command->argv[1];
    if (*text == '%') {
        text++;
    }
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0 || value > 2147483647L) {
        fprintf(stderr, "%s: invalid job number\n", command->argv[0]);
        return -1;
    }
    *job_id = (int)value;
    return 0;
}

static Job *most_recent_job(Shell *shell, bool include_done)
{
    Job *best = NULL;
    for (size_t index = 0; index < SHELL_MAX_JOBS; index++) {
        Job *job = &shell->jobs[index];
        if (job->state == JOB_UNUSED || (!include_done && job->state == JOB_DONE)) {
            continue;
        }
        if (best == NULL || job->id > best->id) {
            best = job;
        }
    }
    return best;
}

static Job *select_job(Shell *shell, int id)
{
    if (id < 0) {
        return most_recent_job(shell, false);
    }
    Job *job = shell_find_job(shell, id);
    if (job != NULL && job->state != JOB_DONE) {
        return job;
    }
    return NULL;
}

int shell_builtin_fg(Shell *shell, const Command *command)
{
    int id;
    if (parse_job_id(command, &id) == -1) {
        return 2;
    }
    shell_reap_jobs(shell);
    Job *job = select_job(shell, id);
    if (job == NULL) {
        fprintf(stderr, "fg: no such job\n");
        return 1;
    }

    if (shell->interactive && tcsetpgrp(shell->terminal_fd,
                                        job->process_group) == -1) {
        perror("fg: tcsetpgrp");
    }
    if (job->state == JOB_STOPPED && kill(-job->process_group, SIGCONT) == -1) {
        perror("fg: SIGCONT");
        shell_restore_terminal(shell);
        return 1;
    }

    int result = shell_wait_for_job(shell, job, true);
    if (job->state == JOB_STOPPED) {
        fprintf(stderr, "[%d] Stopped %s\n", job->id, job->command);
    }
    return result == -1 ? 1 : 0;
}

int shell_builtin_bg(Shell *shell, const Command *command)
{
    int id;
    if (parse_job_id(command, &id) == -1) {
        return 2;
    }
    shell_reap_jobs(shell);
    Job *job = select_job(shell, id);
    if (job == NULL) {
        fprintf(stderr, "bg: no such job\n");
        return 1;
    }

    if (kill(-job->process_group, SIGCONT) == -1) {
        fprintf(stderr, "bg: %s: %s\n", job->command, strerror(errno));
        return 1;
    }
    job->state = JOB_RUNNING;
    printf("[%d] %s &\n", job->id, job->command);
    return 0;
}

static int close_pipes(int pipes[SHELL_MAX_COMMANDS - 1][2], size_t count)
{
    int result = 0;
    for (size_t index = 0; index < count; index++) {
        if (pipes[index][0] >= 0) {
            if (close(pipes[index][0]) == -1) {
                result = -1;
            }
            pipes[index][0] = -1;
        }
        if (pipes[index][1] >= 0) {
            if (close(pipes[index][1]) == -1) {
                result = -1;
            }
            pipes[index][1] = -1;
        }
    }
    return result;
}

static int create_pipes(int pipes[SHELL_MAX_COMMANDS - 1][2], size_t count)
{
    for (size_t index = 0; index < count; index++) {
        pipes[index][0] = -1;
        pipes[index][1] = -1;
    }

    for (size_t index = 0; index < count; index++) {
        if (pipe(pipes[index]) == -1) {
            perror("pipe");
            close_pipes(pipes, count);
            return -1;
        }
    }
    return 0;
}

static int redirect_descriptor(const char *path, int target,
                               bool output, bool append)
{
    int flags = output ? (O_WRONLY | O_CREAT |
                          (append ? O_APPEND : O_TRUNC)) : O_RDONLY;
    int descriptor = open(path, flags, 0666);
    if (descriptor == -1) {
        fprintf(stderr, "redirection: '%s': %s\n", path, strerror(errno));
        return -1;
    }

    if (dup2(descriptor, target) == -1) {
        fprintf(stderr, "redirection: dup2: %s\n", strerror(errno));
        close(descriptor);
        return -1;
    }
    close(descriptor);
    return 0;
}

static int apply_command_redirections(const Command *command)
{
    if (command->input_path != NULL &&
        redirect_descriptor(command->input_path, STDIN_FILENO,
                            false, false) == -1) {
        return -1;
    }
    if (command->output_path != NULL &&
        redirect_descriptor(command->output_path, STDOUT_FILENO,
                            true, command->append_output) == -1) {
        return -1;
    }
    return 0;
}

static void child_close_all_pipes(int pipes[SHELL_MAX_COMMANDS - 1][2],
                                  size_t count)
{
    for (size_t index = 0; index < count; index++) {
        if (pipes[index][0] >= 0) {
            close(pipes[index][0]);
        }
        if (pipes[index][1] >= 0) {
            close(pipes[index][1]);
        }
    }
}

static void run_child_command(Shell *shell, const Pipeline *pipeline,
                              size_t command_index,
                              int pipes[SHELL_MAX_COMMANDS - 1][2],
                              size_t pipe_count, pid_t process_group,
                              const sigset_t *old_mask)
{
    pid_t group = process_group == 0 ? getpid() : process_group;
    if (setpgid(0, group) == -1) {
        perror("setpgid");
        _exit(126);
    }

    restore_child_signal_defaults();
    (void)sigprocmask(SIG_SETMASK, old_mask, NULL);

    if (command_index > 0 &&
        dup2(pipes[command_index - 1][0], STDIN_FILENO) == -1) {
        perror("dup2 stdin");
        _exit(126);
    }
    if (command_index + 1 < pipeline->command_count &&
        dup2(pipes[command_index][1], STDOUT_FILENO) == -1) {
        perror("dup2 stdout");
        _exit(126);
    }

    child_close_all_pipes(pipes, pipe_count);

    const Command *command = &pipeline->commands[command_index];
    if (apply_command_redirections(command) == -1) {
        _exit(126);
    }

    bool handled = false;
    int status = shell_run_builtin(shell, command, false, &handled);
    if (handled) {
        fflush(NULL);
        _exit(status & 0xff);
    }

    execvp(command->argv[0], command->argv);
    fprintf(stderr, "%s: %s\n", command->argv[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

static void close_parent_pipes(int pipes[SHELL_MAX_COMMANDS - 1][2],
                               size_t count)
{
    (void)close_pipes(pipes, count);
}

static int run_parent_builtin(Shell *shell, const Command *command)
{
    bool handled = false;
    int status = shell_run_builtin(shell, command, true, &handled);
    if (!handled) {
        return 127;
    }
    return status;
}

static void copy_command_text(char *destination, size_t capacity,
                              const char *source)
{
    if (capacity == 0) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    (void)snprintf(destination, capacity, "%s", source);
}

int shell_execute(Shell *shell, const Pipeline *pipeline,
                  const char *source_line)
{
    if (shell == NULL || pipeline == NULL ||
        pipeline->command_count == 0) {
        return 0;
    }

    const Command *first = &pipeline->commands[0];
    bool can_run_parent = pipeline->command_count == 1 &&
                          !pipeline->background &&
                          first->input_path == NULL &&
                          first->output_path == NULL &&
                          shell_is_parent_builtin(first);
    if (can_run_parent) {
        return run_parent_builtin(shell, first);
    }

    size_t pipe_count = pipeline->command_count - 1;
    int pipes[SHELL_MAX_COMMANDS - 1][2];
    if (create_pipes(pipes, pipe_count) == -1) {
        return 1;
    }

    sigset_t child_set;
    sigset_t old_mask;
    sigemptyset(&child_set);
    sigaddset(&child_set, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &child_set, &old_mask) == -1) {
        perror("sigprocmask");
        close_parent_pipes(pipes, pipe_count);
        return 1;
    }

    pid_t pids[SHELL_MAX_COMMANDS];
    size_t process_count = 0;
    pid_t process_group = 0;
    bool fork_failed = false;

    for (size_t index = 0; index < pipeline->command_count; index++) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            fork_failed = true;
            break;
        }

        if (pid == 0) {
            run_child_command(shell, pipeline, index, pipes, pipe_count,
                              process_group, &old_mask);
        }

        if (process_group == 0) {
            process_group = pid;
        }
        if (setpgid(pid, process_group) == -1 &&
            errno != EACCES && errno != ESRCH) {
            perror("setpgid");
        }
        pids[process_count++] = pid;
    }

    close_parent_pipes(pipes, pipe_count);

    if (process_count == 0) {
        (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
        return 1;
    }

    if (fork_failed) {
        (void)kill(-process_group, SIGTERM);
    }

    char command_text[SHELL_COMMAND_TEXT];
    copy_command_text(command_text, sizeof(command_text), source_line);
    int job_id = shell_add_job(shell, process_group, pids, process_count,
                               JOB_RUNNING, command_text);
    if (job_id == -1) {
        fprintf(stderr, "shell: job table is full\n");
        (void)kill(-process_group, SIGTERM);
        fork_failed = true;
    }

    Job temporary_job = {0};
    Job *job = job_id == -1 ? &temporary_job : shell_find_job(shell, job_id);
    if (job_id == -1) {
        job->id = 0;
        job->process_group = process_group;
        job->process_count = process_count;
        job->state = JOB_RUNNING;
        memcpy(job->processes, pids, process_count * sizeof(*pids));
    }

    if (pipeline->background && !fork_failed) {
        printf("[%d] %ld\n", job->id, (long)process_group);
        job->state = JOB_RUNNING;
        (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
        return 0;
    }

    if (job->id != 0 && !pipeline->background) {
        job->state = JOB_RUNNING;
    }
    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);

    if (shell_wait_for_job(shell, job, true) == -1) {
        return 1;
    }

    if (job->state == JOB_STOPPED && job->id > 0) {
        fprintf(stderr, "[%d] Stopped %s\n", job->id, job->command);
    }
    return job->state == JOB_DONE ? 0 : 128 + SIGTSTP;
}
