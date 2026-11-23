#define _DEFAULT_SOURCE
#include "shell.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * Command implementations in this file operate on files and directories
 * through POSIX interfaces.  This keeps the required commands independent
 * from similarly named programs installed elsewhere on the system.
 */

static int report_error(const char *operation, const char *path)
{
    if (path == NULL) {
        fprintf(stderr, "%s: %s\n", operation, strerror(errno));
    } else {
        fprintf(stderr, "%s: '%s': %s\n", operation, path,
                strerror(errno));
    }
    return 1;
}

static bool option_is(const char *argument, const char *short_name,
                      const char *long_name)
{
    return strcmp(argument, short_name) == 0 ||
           strcmp(argument, long_name) == 0;
}

static int write_all(int descriptor, const void *data, size_t length)
{
    const unsigned char *cursor = data;
    size_t written = 0;

    while (written < length) {
        ssize_t amount = write(descriptor, cursor + written,
                               length - written);
        if (amount == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (amount == 0) {
            errno = EIO;
            return -1;
        }
        written += (size_t)amount;
    }
    return 0;
}

static int copy_file_contents(const char *source, const char *destination,
                              mode_t mode)
{
    int input = open(source, O_RDONLY);
    if (input == -1) {
        return report_error("cp", source);
    }

    int output = open(destination, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (output == -1) {
        int saved_errno = errno;
        close(input);
        errno = saved_errno;
        return report_error("cp", destination);
    }

    unsigned char buffer[16384];
    int result = 0;

    for (;;) {
        ssize_t amount = read(input, buffer, sizeof(buffer));
        if (amount == 0) {
            break;
        }
        if (amount == -1) {
            if (errno == EINTR) {
                continue;
            }
            result = report_error("cp", source);
            break;
        }
        if (write_all(output, buffer, (size_t)amount) == -1) {
            result = report_error("cp", destination);
            break;
        }
    }

    if (close(input) == -1 && result == 0) {
        result = report_error("cp", source);
    }
    if (close(output) == -1 && result == 0) {
        result = report_error("cp", destination);
    }
    return result;
}

static int path_is_directory(const char *path)
{
    struct stat information;
    if (stat(path, &information) == -1) {
        return -1;
    }
    return S_ISDIR(information.st_mode) ? 1 : 0;
}

static int path_is_same_file(const char *left, const char *right)
{
    struct stat left_stat;
    struct stat right_stat;

    if (stat(left, &left_stat) == -1 || stat(right, &right_stat) == -1) {
        return 0;
    }
    return left_stat.st_dev == right_stat.st_dev &&
           left_stat.st_ino == right_stat.st_ino;
}

static int make_parent_directories(const char *path, mode_t mode)
{
    char buffer[PATH_MAX];
    size_t length = strlen(path);

    if (length == 0 || length >= sizeof(buffer)) {
        errno = length == 0 ? EINVAL : ENAMETOOLONG;
        return -1;
    }

    memcpy(buffer, path, length + 1);

    for (char *cursor = buffer + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(buffer, mode) == -1 && errno != EEXIST) {
                return -1;
            }
            *cursor = '/';
        }
    }
    return 0;
}

static int copy_directory_contents(const char *source,
                                   const char *destination, mode_t mode,
                                   bool recursive)
{
    DIR *directory = opendir(source);
    if (directory == NULL) {
        return report_error("cp", source);
    }

    if (mkdir(destination, mode) == -1 && errno != EEXIST) {
        int saved_errno = errno;
        closedir(directory);
        errno = saved_errno;
        return report_error("cp", destination);
    }

    int result = 0;
    struct dirent *entry;

    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char source_child[PATH_MAX];
        char destination_child[PATH_MAX];
        int source_length = snprintf(source_child, sizeof(source_child),
                                     "%s/%s", source, entry->d_name);
        int destination_length = snprintf(destination_child,
                                          sizeof(destination_child),
                                          "%s/%s", destination,
                                          entry->d_name);
        if (source_length < 0 || (size_t)source_length >= sizeof(source_child) ||
            destination_length < 0 ||
            (size_t)destination_length >= sizeof(destination_child)) {
            errno = ENAMETOOLONG;
            result = report_error("cp", source);
            continue;
        }

        struct stat information;
        if (lstat(source_child, &information) == -1) {
            result = report_error("cp", source_child);
            continue;
        }

        if (S_ISDIR(information.st_mode)) {
            if (!recursive) {
                fprintf(stderr, "cp: '%s' is a directory (use -r)\n",
                        source_child);
                result = 1;
                continue;
            }
            if (copy_directory_contents(source_child, destination_child,
                                        information.st_mode & 0777,
                                        recursive) != 0) {
                result = 1;
            }
        } else if (S_ISLNK(information.st_mode)) {
            char link_target[PATH_MAX];
            ssize_t target_length = readlink(source_child, link_target,
                                             sizeof(link_target) - 1);
            if (target_length == -1) {
                result = report_error("cp", source_child);
                continue;
            }
            link_target[target_length] = '\0';
            if (symlink(link_target, destination_child) == -1) {
                result = report_error("cp", destination_child);
            }
        } else if (copy_file_contents(source_child, destination_child,
                                      information.st_mode & 0777) != 0) {
            result = 1;
        }
    }

    if (closedir(directory) == -1 && result == 0) {
        result = report_error("cp", source);
    }
    return result;
}

static int copy_path(const char *source, const char *destination,
                     bool recursive)
{
    struct stat information;
    if (lstat(source, &information) == -1) {
        return report_error("cp", source);
    }

    if (strcmp(source, destination) == 0 ||
        path_is_same_file(source, destination)) {
        errno = EINVAL;
        return report_error("cp", destination);
    }

    if (S_ISDIR(information.st_mode)) {
        if (!recursive) {
            fprintf(stderr, "cp: '%s' is a directory (use -r)\n", source);
            return 1;
        }
        return copy_directory_contents(source, destination,
                                       information.st_mode & 0777, recursive);
    }

    if (S_ISLNK(information.st_mode)) {
        char target[PATH_MAX];
        ssize_t amount = readlink(source, target, sizeof(target) - 1);
        if (amount == -1) {
            return report_error("cp", source);
        }
        target[amount] = '\0';
        if (symlink(target, destination) == -1) {
            return report_error("cp", destination);
        }
        return 0;
    }

    if (!S_ISREG(information.st_mode)) {
        fprintf(stderr, "cp: unsupported file type: '%s'\n", source);
        return 1;
    }

    return copy_file_contents(source, destination,
                              information.st_mode & 0777);
}

static int remove_path(const char *path, bool recursive, bool force)
{
    struct stat information;

    if (lstat(path, &information) == -1) {
        if (force && errno == ENOENT) {
            return 0;
        }
        return report_error("rm", path);
    }

    if (S_ISDIR(information.st_mode)) {
        if (!recursive) {
            fprintf(stderr, "rm: cannot remove '%s': is a directory\n", path);
            return 1;
        }

        DIR *directory = opendir(path);
        if (directory == NULL) {
            return report_error("rm", path);
        }

        int result = 0;
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char child[PATH_MAX];
            int amount = snprintf(child, sizeof(child), "%s/%s", path,
                                  entry->d_name);
            if (amount < 0 || (size_t)amount >= sizeof(child)) {
                errno = ENAMETOOLONG;
                result = report_error("rm", path);
                continue;
            }
            if (remove_path(child, recursive, force) != 0) {
                result = 1;
            }
        }
        closedir(directory);

        if (rmdir(path) == -1) {
            if (!(force && errno == ENOENT)) {
                result = report_error("rm", path);
            }
        }
        return result;
    }

    if (unlink(path) == -1) {
        if (force && errno == ENOENT) {
            return 0;
        }
        return report_error("rm", path);
    }
    return 0;
}

static void print_mode(mode_t mode)
{
    char type = '-';
    if (S_ISDIR(mode)) {
        type = 'd';
    } else if (S_ISLNK(mode)) {
        type = 'l';
    } else if (S_ISCHR(mode)) {
        type = 'c';
    } else if (S_ISBLK(mode)) {
        type = 'b';
    } else if (S_ISFIFO(mode)) {
        type = 'p';
    } else if (S_ISSOCK(mode)) {
        type = 's';
    }

    char permissions[10] = {
        (mode & S_IRUSR) ? 'r' : '-',
        (mode & S_IWUSR) ? 'w' : '-',
        (mode & S_IXUSR) ? 'x' : '-',
        (mode & S_IRGRP) ? 'r' : '-',
        (mode & S_IWGRP) ? 'w' : '-',
        (mode & S_IXGRP) ? 'x' : '-',
        (mode & S_IROTH) ? 'r' : '-',
        (mode & S_IWOTH) ? 'w' : '-',
        (mode & S_IXOTH) ? 'x' : '-',
    };
    printf("%c%s", type, permissions);
}

static int print_long_entry(const char *path, const char *name)
{
    struct stat information;
    if (lstat(path, &information) == -1) {
        return report_error("ls", path);
    }

    print_mode(information.st_mode);
    printf(" %3lu %8ld ", (unsigned long)information.st_nlink,
           (long)information.st_size);

    char time_text[64];
    struct tm *local_time = localtime(&information.st_mtime);
    if (local_time == NULL ||
        strftime(time_text, sizeof(time_text), "%b %d %H:%M", local_time) == 0) {
        (void)snprintf(time_text, sizeof(time_text), "unknown time");
    }
    printf("%s %s", time_text, name);

    if (S_ISLNK(information.st_mode)) {
        char target[PATH_MAX];
        ssize_t amount = readlink(path, target, sizeof(target) - 1);
        if (amount >= 0) {
            target[amount] = '\0';
            printf(" -> %s", target);
        }
    }
    putchar('\n');
    return 0;
}

static int join_path(char *result, size_t size, const char *directory,
                     const char *name)
{
    int amount = snprintf(result, size, "%s/%s", directory, name);
    if (amount < 0 || (size_t)amount >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int list_directory(const char *path, const char *display_name,
                          bool show_all, bool long_format, bool recursive)
{
    DIR *directory = opendir(path);
    if (directory == NULL) {
        return report_error("ls", path);
    }

    if (display_name != NULL) {
        printf("%s:\n", display_name);
    }

    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!show_all && entry->d_name[0] == '.') {
            continue;
        }

        char child_path[PATH_MAX];
        if (join_path(child_path, sizeof(child_path), path,
                      entry->d_name) == -1) {
            result = report_error("ls", path);
            continue;
        }

        if (long_format) {
            if (print_long_entry(child_path, entry->d_name) != 0) {
                result = 1;
            }
        } else {
            printf("%s  ", entry->d_name);
        }
    }
    if (!long_format) {
        putchar('\n');
    }
    closedir(directory);

    if (recursive) {
        directory = opendir(path);
        if (directory == NULL) {
            return result == 0 ? report_error("ls", path) : result;
        }
        while ((entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char child_path[PATH_MAX];
            if (join_path(child_path, sizeof(child_path), path,
                          entry->d_name) == -1) {
                result = report_error("ls", path);
                continue;
            }

            struct stat information;
            if (lstat(child_path, &information) == -1) {
                result = report_error("ls", child_path);
                continue;
            }
            if (S_ISDIR(information.st_mode)) {
                putchar('\n');
                if (list_directory(child_path, child_path, show_all,
                                   long_format, recursive) != 0) {
                    result = 1;
                }
            }
        }
        closedir(directory);
    }

    return result;
}

static int command_ls_one(const char *path, bool show_all, bool long_format,
                          bool recursive)
{
    struct stat information;
    if (lstat(path, &information) == -1) {
        return report_error("ls", path);
    }
    if (S_ISDIR(information.st_mode)) {
        return list_directory(path, NULL, show_all, long_format, recursive);
    }
    if (long_format) {
        return print_long_entry(path, path);
    }
    printf("%s\n", path);
    return 0;
}

int command_ls(const Command *command)
{
    bool show_all = false;
    bool long_format = false;
    bool recursive = false;
    const char *paths[SHELL_MAX_ARGS];
    size_t path_count = 0;

    for (size_t index = 1; index < command->argc; index++) {
        const char *argument = command->argv[index];
        if (strcmp(argument, "--") == 0) {
            for (index++; index < command->argc; index++) {
                paths[path_count++] = command->argv[index];
            }
            break;
        }
        if (argument[0] == '-' && argument[1] != '\0') {
            for (size_t offset = 1; argument[offset] != '\0'; offset++) {
                if (argument[offset] == 'a') {
                    show_all = true;
                } else if (argument[offset] == 'l') {
                    long_format = true;
                } else if (argument[offset] == 'R') {
                    recursive = true;
                } else {
                    fprintf(stderr, "ls: unsupported option: -%c\n",
                            argument[offset]);
                    return 2;
                }
            }
        } else {
            paths[path_count++] = argument;
        }
    }

    if (path_count == 0) {
        return command_ls_one(".", show_all, long_format, recursive);
    }

    int result = 0;
    for (size_t index = 0; index < path_count; index++) {
        if (path_count > 1) {
            putchar('\n');
            printf("%s:\n", paths[index]);
        }
        if (command_ls_one(paths[index], show_all, long_format,
                           recursive) != 0) {
            result = 1;
        }
    }
    return result;
}

int command_pwd(const Command *command)
{
    (void)command;
    char directory[PATH_MAX];

    if (getcwd(directory, sizeof(directory)) == NULL) {
        return report_error("pwd", NULL);
    }
    puts(directory);
    return 0;
}

int command_mkdir(const Command *command)
{
    bool parents = false;
    size_t first_path = 1;

    while (first_path < command->argc && command->argv[first_path][0] == '-') {
        if (option_is(command->argv[first_path], "-p", "--parents")) {
            parents = true;
            first_path++;
        } else if (strcmp(command->argv[first_path], "--") == 0) {
            first_path++;
            break;
        } else {
            fprintf(stderr, "mkdir: unsupported option: %s\n",
                    command->argv[first_path]);
            return 2;
        }
    }

    if (first_path == command->argc) {
        fprintf(stderr, "mkdir: missing operand\n");
        return 2;
    }

    int result = 0;
    for (size_t index = first_path; index < command->argc; index++) {
        const char *path = command->argv[index];
        if (parents) {
            if (make_parent_directories(path, 0777) == -1) {
                result = report_error("mkdir", path);
            }
        }
        if (mkdir(path, 0777) == -1) {
            if (!(parents && errno == EEXIST && path_is_directory(path) == 1)) {
                result = report_error("mkdir", path);
            }
        }
    }
    return result;
}

int command_rmdir(const Command *command)
{
    bool parents = false;
    size_t first_path = 1;

    while (first_path < command->argc && command->argv[first_path][0] == '-') {
        if (option_is(command->argv[first_path], "-p", "--parents")) {
            parents = true;
            first_path++;
        } else if (strcmp(command->argv[first_path], "--") == 0) {
            first_path++;
            break;
        } else {
            fprintf(stderr, "rmdir: unsupported option: %s\n",
                    command->argv[first_path]);
            return 2;
        }
    }

    if (first_path == command->argc) {
        fprintf(stderr, "rmdir: missing operand\n");
        return 2;
    }

    int result = 0;
    for (size_t index = first_path; index < command->argc; index++) {
        char path[PATH_MAX];
        size_t length = strlen(command->argv[index]);
        if (length >= sizeof(path)) {
            errno = ENAMETOOLONG;
            result = report_error("rmdir", command->argv[index]);
            continue;
        }
        memcpy(path, command->argv[index], length + 1);

        if (rmdir(path) == -1) {
            result = report_error("rmdir", path);
            continue;
        }

        if (parents) {
            char *slash = strrchr(path, '/');
            while (slash != NULL) {
                if (slash == path) {
                    if (path[1] == '\0') {
                        break;
                    }
                    slash[1] = '\0';
                } else {
                    *slash = '\0';
                }
                if (rmdir(path) == -1) {
                    break;
                }
                slash = strrchr(path, '/');
            }
        }
    }
    return result;
}

int command_ln(const Command *command)
{
    bool symbolic = false;
    bool force = false;
    size_t index = 1;

    while (index < command->argc && command->argv[index][0] == '-') {
        if (strcmp(command->argv[index], "-s") == 0 ||
            strcmp(command->argv[index], "--symbolic") == 0) {
            symbolic = true;
        } else if (strcmp(command->argv[index], "-f") == 0 ||
                   strcmp(command->argv[index], "--force") == 0) {
            force = true;
        } else if (strcmp(command->argv[index], "--") == 0) {
            index++;
            break;
        } else {
            fprintf(stderr, "ln: unsupported option: %s\n",
                    command->argv[index]);
            return 2;
        }
        index++;
    }

    if (command->argc - index != 2) {
        fprintf(stderr, "usage: ln [-s] [-f] TARGET LINK_NAME\n");
        return 2;
    }

    const char *target = command->argv[index];
    const char *link_name = command->argv[index + 1];
    if (force && unlink(link_name) == -1 && errno != ENOENT) {
        return report_error("ln", link_name);
    }

    int result = symbolic ? symlink(target, link_name) : link(target, link_name);
    if (result == -1) {
        return report_error("ln", link_name);
    }
    return 0;
}

static int destination_for_source(const char *source, const char *destination,
                                  char *result, size_t result_size)
{
    int directory = path_is_directory(destination);
    if (directory == -1) {
        if (errno != ENOENT) {
            return -1;
        }
        int amount = snprintf(result, result_size, "%s", destination);
        if (amount < 0 || (size_t)amount >= result_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    const char *base = strrchr(source, '/');
    base = base == NULL ? source : base + 1;
    return join_path(result, result_size, destination, base);
}

int command_cp(const Command *command)
{
    bool recursive = false;
    size_t index = 1;

    while (index < command->argc && command->argv[index][0] == '-') {
        if (strcmp(command->argv[index], "-r") == 0 ||
            strcmp(command->argv[index], "-R") == 0 ||
            strcmp(command->argv[index], "--recursive") == 0) {
            recursive = true;
        } else if (strcmp(command->argv[index], "--") == 0) {
            index++;
            break;
        } else {
            fprintf(stderr, "cp: unsupported option: %s\n",
                    command->argv[index]);
            return 2;
        }
        index++;
    }

    if (command->argc - index != 2) {
        fprintf(stderr, "usage: cp [-r] SOURCE DESTINATION\n");
        return 2;
    }

    const char *source = command->argv[index];
    const char *destination = command->argv[index + 1];
    char final_destination[PATH_MAX];

    if (destination_for_source(source, destination, final_destination,
                               sizeof(final_destination)) == -1) {
        return report_error("cp", destination);
    }
    return copy_path(source, final_destination, recursive);
}

int command_rm(const Command *command)
{
    bool recursive = false;
    bool force = false;
    size_t first_path = 1;

    while (first_path < command->argc && command->argv[first_path][0] == '-') {
        const char *option = command->argv[first_path];
        if (strcmp(option, "--") == 0) {
            first_path++;
            break;
        }
        if (strcmp(option, "--recursive") == 0) {
            recursive = true;
        } else if (strcmp(option, "--force") == 0) {
            force = true;
        } else if (option[1] != '\0') {
            for (size_t offset = 1; option[offset] != '\0'; offset++) {
                if (option[offset] == 'r' || option[offset] == 'R') {
                    recursive = true;
                } else if (option[offset] == 'f') {
                    force = true;
                } else {
                    fprintf(stderr, "rm: unsupported option: -%c\n",
                            option[offset]);
                    return 2;
                }
            }
        }
        first_path++;
    }

    if (first_path == command->argc) {
        if (force) {
            return 0;
        }
        fprintf(stderr, "rm: missing operand\n");
        return 2;
    }

    int result = 0;
    for (size_t index = first_path; index < command->argc; index++) {
        if (remove_path(command->argv[index], recursive, force) != 0) {
            result = 1;
        }
    }
    return result;
}

int command_mv(const Command *command)
{
    if (command->argc != 3) {
        fprintf(stderr, "usage: mv SOURCE DESTINATION\n");
        return 2;
    }

    const char *source = command->argv[1];
    const char *destination = command->argv[2];
    char final_destination[PATH_MAX];

    if (destination_for_source(source, destination, final_destination,
                               sizeof(final_destination)) == -1) {
        return report_error("mv", destination);
    }
    if (path_is_same_file(source, final_destination)) {
        errno = EINVAL;
        return report_error("mv", final_destination);
    }

    if (rename(source, final_destination) == 0) {
        return 0;
    }
    if (errno != EXDEV) {
        return report_error("mv", final_destination);
    }

    if (copy_path(source, final_destination, true) != 0) {
        return 1;
    }
    return remove_path(source, true, false);
}

static int cat_stream(FILE *input, const char *name, bool number_lines,
                      bool *first_line)
{
    char *line = NULL;
    size_t capacity = 0;
    ssize_t amount;
    unsigned long line_number = 0;
    int result = 0;

    while ((amount = getline(&line, &capacity, input)) != -1) {
        line_number++;
        if (number_lines) {
            if (printf("%6lu\t", line_number) < 0) {
                result = 1;
                break;
            }
        }
        if (fwrite(line, 1, (size_t)amount, stdout) != (size_t)amount) {
            result = report_error("cat", name);
            break;
        }
        *first_line = false;
    }

    if (ferror(input)) {
        result = report_error("cat", name);
    }
    free(line);
    return result;
}

int command_cat(const Command *command)
{
    bool number_lines = false;
    size_t first_file = 1;

    while (first_file < command->argc && command->argv[first_file][0] == '-') {
        if (strcmp(command->argv[first_file], "-n") == 0 ||
            strcmp(command->argv[first_file], "--number") == 0) {
            number_lines = true;
        } else if (strcmp(command->argv[first_file], "--") == 0) {
            first_file++;
            break;
        } else {
            fprintf(stderr, "cat: unsupported option: %s\n",
                    command->argv[first_file]);
            return 2;
        }
        first_file++;
    }

    bool first_line = true;
    if (first_file == command->argc) {
        return cat_stream(stdin, "standard input", number_lines, &first_line);
    }

    int result = 0;
    for (size_t index = first_file; index < command->argc; index++) {
        FILE *input = fopen(command->argv[index], "rb");
        if (input == NULL) {
            result = report_error("cat", command->argv[index]);
            continue;
        }
        if (cat_stream(input, command->argv[index], number_lines,
                       &first_line) != 0) {
            result = 1;
        }
        if (fclose(input) == EOF) {
            result = report_error("cat", command->argv[index]);
        }
    }
    return result;
}

static int grep_stream(regex_t *expression, FILE *input, const char *name,
                       bool show_line_number, bool invert, bool count_only,
                       bool list_files)
{
    char *line = NULL;
    size_t capacity = 0;
    ssize_t amount;
    unsigned long line_number = 0;
    unsigned long matches = 0;
    int result = 0;

    while ((amount = getline(&line, &capacity, input)) != -1) {
        line_number++;
        int match = regexec(expression, line, 0, NULL, 0) == 0;
        if (invert) {
            match = !match;
        }
        if (!match) {
            continue;
        }

        matches++;
        if (list_files) {
            break;
        }
        if (!count_only) {
            if (name != NULL) {
                printf("%s:", name);
            }
            if (show_line_number) {
                printf("%lu:", line_number);
            }
            if (fwrite(line, 1, (size_t)amount, stdout) != (size_t)amount) {
                result = report_error("grep", name);
                break;
            }
            if (amount == 0 || line[amount - 1] != '\n') {
                putchar('\n');
            }
        }
    }

    if (ferror(input)) {
        result = report_error("grep", name);
    } else if (list_files && matches > 0) {
        puts(name == NULL ? "(standard input)" : name);
    } else if (count_only) {
        if (name != NULL) {
            printf("%s:", name);
        }
        printf("%lu\n", matches);
    }

    free(line);
    return result == 0 && matches == 0 ? 1 : result;
}

int command_grep(const Command *command)
{
    bool ignore_case = false;
    bool invert = false;
    bool show_line_number = false;
    bool count_only = false;
    bool list_files = false;
    int regex_flags = 0;
    size_t index = 1;

    while (index < command->argc && command->argv[index][0] == '-') {
        const char *option = command->argv[index];
        if (strcmp(option, "--") == 0) {
            index++;
            break;
        }
        if (strcmp(option, "--ignore-case") == 0) {
            ignore_case = true;
        } else if (strcmp(option, "--invert-match") == 0) {
            invert = true;
        } else if (strcmp(option, "--line-number") == 0) {
            show_line_number = true;
        } else if (strcmp(option, "--count") == 0) {
            count_only = true;
        } else if (strcmp(option, "--files-with-matches") == 0) {
            list_files = true;
        } else {
            for (size_t offset = 1; option[offset] != '\0'; offset++) {
                if (option[offset] == 'i') {
                    ignore_case = true;
                } else if (option[offset] == 'v') {
                    invert = true;
                } else if (option[offset] == 'n') {
                    show_line_number = true;
                } else if (option[offset] == 'c') {
                    count_only = true;
                } else if (option[offset] == 'l') {
                    list_files = true;
                } else if (option[offset] == 'E') {
                    regex_flags |= REG_EXTENDED;
                } else {
                    fprintf(stderr, "grep: unsupported option: -%c\n",
                            option[offset]);
                    return 2;
                }
            }
        }
        index++;
    }

    if (index >= command->argc) {
        fprintf(stderr, "usage: grep [OPTIONS] PATTERN [FILE ...]\n");
        return 2;
    }

    const char *pattern = command->argv[index++];
    if (ignore_case) {
        regex_flags |= REG_ICASE;
    }

    regex_t expression;
    int regex_result = regcomp(&expression, pattern, regex_flags);
    if (regex_result != 0) {
        char message[256];
        regerror(regex_result, &expression, message, sizeof(message));
        fprintf(stderr, "grep: invalid pattern: %s\n", message);
        return 2;
    }

    int result = 1;
    if (index == command->argc) {
        result = grep_stream(&expression, stdin, NULL, show_line_number,
                             invert, count_only, list_files);
    } else {
        for (; index < command->argc; index++) {
            FILE *input = fopen(command->argv[index], "r");
            if (input == NULL) {
                result = report_error("grep", command->argv[index]);
                continue;
            }

            int current = grep_stream(&expression, input,
                                      command->argv[index], show_line_number,
                                      invert, count_only, list_files);
            if (current == 0) {
                result = 0;
            } else if (current > 1 && result != 0) {
                result = current;
            }
            fclose(input);
        }
    }

    regfree(&expression);
    return result;
}

static int builtin_cd(Shell *shell, const Command *command, bool in_parent)
{
    const char *target = NULL;

    if (command->argc == 1) {
        target = getenv("HOME");
        if (target == NULL) {
            fprintf(stderr, "cd: HOME is not set\n");
            return 1;
        }
    } else if (command->argc == 2 && strcmp(command->argv[1], "-") == 0) {
        target = shell->old_directory;
        if (target == NULL) {
            target = getenv("OLDPWD");
        }
        if (target == NULL) {
            fprintf(stderr, "cd: OLDPWD is not set\n");
            return 1;
        }
        puts(target);
    } else if (command->argc == 2) {
        target = command->argv[1];
    } else {
        fprintf(stderr, "cd: too many arguments\n");
        return 2;
    }

    char current[PATH_MAX];
    if (getcwd(current, sizeof(current)) == NULL) {
        return report_error("cd", NULL);
    }

    if (chdir(target) == -1) {
        return report_error("cd", target);
    }

    if (in_parent) {
        free(shell->old_directory);
        shell->old_directory = strdup(current);
        if (shell->old_directory != NULL) {
            (void)setenv("OLDPWD", shell->old_directory, 1);
        }
        char new_directory[PATH_MAX];
        if (getcwd(new_directory, sizeof(new_directory)) != NULL) {
            (void)setenv("PWD", new_directory, 1);
        }
    }

    return 0;
}

static int builtin_exit(Shell *shell, const Command *command, bool in_parent)
{
    int status = shell->last_status;
    if (command->argc > 2) {
        fprintf(stderr, "exit: too many arguments\n");
        return 2;
    }
    if (command->argc == 2) {
        char *end = NULL;
        errno = 0;
        long parsed = strtol(command->argv[1], &end, 10);
        if (errno != 0 || end == command->argv[1] || *end != '\0') {
            fprintf(stderr, "exit: numeric status required\n");
            return 2;
        }
        status = (int)(parsed & 0xff);
    }
    if (in_parent) {
        shell->should_exit = true;
        shell->exit_status = status;
    }
    return status;
}

static int run_command_by_name(Shell *shell, const Command *command,
                               bool in_parent, bool *handled)
{
    (void)shell;
    (void)in_parent;
    *handled = true;

    if (strcmp(command->argv[0], "ls") == 0) {
        return command_ls(command);
    }
    if (strcmp(command->argv[0], "pwd") == 0) {
        return command_pwd(command);
    }
    if (strcmp(command->argv[0], "mkdir") == 0) {
        return command_mkdir(command);
    }
    if (strcmp(command->argv[0], "rmdir") == 0) {
        return command_rmdir(command);
    }
    if (strcmp(command->argv[0], "ln") == 0) {
        return command_ln(command);
    }
    if (strcmp(command->argv[0], "cp") == 0) {
        return command_cp(command);
    }
    if (strcmp(command->argv[0], "rm") == 0) {
        return command_rm(command);
    }
    if (strcmp(command->argv[0], "mv") == 0) {
        return command_mv(command);
    }
    if (strcmp(command->argv[0], "cat") == 0) {
        return command_cat(command);
    }
    if (strcmp(command->argv[0], "grep") == 0) {
        return command_grep(command);
    }

    *handled = false;
    return 0;
}

bool shell_is_parent_builtin(const Command *command)
{
    if (command == NULL || command->argc == 0) {
        return false;
    }
    return strcmp(command->argv[0], "cd") == 0 ||
           strcmp(command->argv[0], "exit") == 0 ||
           strcmp(command->argv[0], "jobs") == 0 ||
           strcmp(command->argv[0], "fg") == 0 ||
           strcmp(command->argv[0], "bg") == 0;
}

int shell_run_builtin(Shell *shell, const Command *command,
                      bool in_parent, bool *handled)
{
    if (command == NULL || command->argc == 0) {
        *handled = true;
        return 0;
    }

    *handled = true;

    if (strcmp(command->argv[0], "cd") == 0) {
        return builtin_cd(shell, command, in_parent);
    }
    if (strcmp(command->argv[0], "exit") == 0) {
        return builtin_exit(shell, command, in_parent);
    }
    if (strcmp(command->argv[0], "jobs") == 0) {
        return shell_builtin_jobs(shell, command);
    }
    if (strcmp(command->argv[0], "fg") == 0) {
        return shell_builtin_fg(shell, command);
    }
    if (strcmp(command->argv[0], "bg") == 0) {
        return shell_builtin_bg(shell, command);
    }

    int result = run_command_by_name(shell, command, in_parent, handled);
    return result;
}
