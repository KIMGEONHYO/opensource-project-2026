#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_LINE 1024
#define MAX_ARGS 64

static void reap_children(int signal_number)
{
    int saved_errno = errno;
    (void)signal_number;

    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }

    errno = saved_errno;
}

static int parse_line(char *line, char *argv[], size_t max_args)
{
    size_t argc = 0;
    char *saveptr = NULL;

    for (char *token = strtok_r(line, " \t\r\n", &saveptr);
         token != NULL;
         token = strtok_r(NULL, " \t\r\n", &saveptr)) {
        if (argc + 1 >= max_args) {
            fprintf(stderr, "Too many arguments\n");
            return -1;
        }
        argv[argc++] = token;
    }

    argv[argc] = NULL;
    return (int)argc;
}

int main(void)
{
    struct sigaction action = {0};

    action.sa_handler = reap_children;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &action, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    char line[MAX_LINE];
    char *argv[MAX_ARGS];

    while (1) {
        printf("mysh> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (feof(stdin)) {
                putchar('\n');
                break;
            }
            perror("fgets");
            clearerr(stdin);
            continue;
        }

        int argc = parse_line(line, argv, MAX_ARGS);
        if (argc <= 0) {
            continue;
        }

        int background = 0;
        size_t last_length = strlen(argv[argc - 1]);

        if (last_length > 0 && argv[argc - 1][last_length - 1] == '&') {
            background = 1;
            argv[argc - 1][last_length - 1] = '\0';

            if (argv[argc - 1][0] == '\0') {
                argc--;
            }
            argv[argc] = NULL;
        }

        if (argc == 0) {
            continue;
        }

        if (strcmp(argv[0], "exit") == 0) {
            if (background) {
                fprintf(stderr, "exit cannot run in the background\n");
                continue;
            }
            break;
        }

        if (strcmp(argv[0], "cd") == 0) {
            if (background) {
                fprintf(stderr, "cd cannot run in the background\n");
                continue;
            }

            const char *path = (argc >= 2) ? argv[1] : getenv("HOME");

            if (path == NULL || chdir(path) == -1) {
                perror("cd");
            }
            continue;
        }

        sigset_t child_set;
        sigset_t old_set;

        sigemptyset(&child_set);
        sigaddset(&child_set, SIGCHLD);

        if (sigprocmask(SIG_BLOCK, &child_set, &old_set) == -1) {
            perror("sigprocmask");
            continue;
        }

        pid_t pid = fork();

        if (pid == -1) {
            perror("fork");
            sigprocmask(SIG_SETMASK, &old_set, NULL);
            continue;
        }

        if (pid == 0) {
            sigprocmask(SIG_SETMASK, &old_set, NULL);
            execvp(argv[0], argv);
            perror(argv[0]);
            _exit(127);
        }

        if (background) {
            printf("[background pid %ld]\n", (long)pid);
        } else {
            while (waitpid(pid, NULL, 0) == -1) {
                if (errno == EINTR) {
                    continue;
                }
                perror("waitpid");
                break;
            }
        }

        if (sigprocmask(SIG_SETMASK, &old_set, NULL) == -1) {
            perror("sigprocmask");
        }
    }

    return 0;
}
