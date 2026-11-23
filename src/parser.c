#include "shell.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The lexer intentionally handles the shell syntax needed by this project:
 * whitespace, single and double quotes, backslash escapes, pipes, input and
 * output redirection, append redirection, and a trailing background marker.
 * It is not intended to implement every rule of POSIX shell grammar.
 */

static void set_error(char *error, size_t size, const char *message)
{
    if (error != NULL && size > 0) {
        (void)snprintf(error, size, "%s", message);
    }
}

static int append_character(char *buffer, size_t capacity, size_t *length,
                            char value)
{
    if (*length + 1 >= capacity) {
        return -1;
    }
    buffer[*length] = value;
    *length += 1;
    buffer[*length] = '\0';
    return 0;
}

static int append_string(char *buffer, size_t capacity, size_t *length,
                         const char *value)
{
    if (value == NULL) {
        return 0;
    }

    while (*value != '\0') {
        if (append_character(buffer, capacity, length, *value) == -1) {
            return -1;
        }
        value++;
    }
    return 0;
}

static int add_token(ParsedLine *parsed, TokenKind kind, const char *text,
                     char *error, size_t error_size)
{
    if (parsed->token_count >= SHELL_MAX_TOKENS) {
        set_error(error, error_size, "too many tokens");
        return -1;
    }

    Token *token = &parsed->tokens[parsed->token_count];
    token->kind = kind;
    token->text = NULL;

    if (text != NULL) {
        token->text = strdup(text);
        if (token->text == NULL) {
            set_error(error, error_size, "out of memory while parsing");
            return -1;
        }
    }

    parsed->token_count++;
    return 0;
}

static bool is_operator_character(char value)
{
    return value == '|' || value == '<' || value == '>' || value == '&';
}

static int lex_variable(const char **input, char *buffer, size_t capacity,
                        size_t *length)
{
    const char *cursor = *input;
    char name[256];
    size_t name_length = 0;

    cursor++; /* Skip the dollar sign. */

    if (*cursor == '{') {
        cursor++;
        while (*cursor != '\0' && *cursor != '}') {
            if (name_length + 1 >= sizeof(name)) {
                return -1;
            }
            name[name_length++] = *cursor++;
        }
        if (*cursor != '}') {
            return -1;
        }
        cursor++;
    } else {
        while (isalnum((unsigned char)*cursor) || *cursor == '_') {
            if (name_length + 1 >= sizeof(name)) {
                return -1;
            }
            name[name_length++] = *cursor++;
        }
    }

    if (name_length == 0) {
        if (append_character(buffer, capacity, length, '$') == -1) {
            return -1;
        }
        *input = cursor;
        return 0;
    }

    name[name_length] = '\0';
    const char *value = getenv(name);
    if (append_string(buffer, capacity, length, value) == -1) {
        return -1;
    }

    *input = cursor;
    return 0;
}

static int lex_word(const char **input, ParsedLine *parsed,
                    char *error, size_t error_size)
{
    char word[SHELL_MAX_LINE * 4];
    size_t length = 0;
    char quote = '\0';
    bool began = false;
    const char *cursor = *input;
    word[0] = '\0';

    if (*cursor == '~' && (cursor[1] == '/' || cursor[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home != NULL && append_string(word, sizeof(word), &length, home) == -1) {
            set_error(error, error_size, "word is too long");
            return -1;
        }
        cursor++;
        began = true;
    }

    while (*cursor != '\0') {
        char value = *cursor;

        if (quote == '\0' && (isspace((unsigned char)value) ||
                               is_operator_character(value))) {
            break;
        }

        if (quote == '\0' && (value == '\'' || value == '"')) {
            quote = value;
            began = true;
            cursor++;
            continue;
        }

        if (quote != '\0' && value == quote) {
            quote = '\0';
            cursor++;
            continue;
        }

        if (quote != '\'' && value == '\\' && cursor[1] != '\0') {
            cursor++;
            if (append_character(word, sizeof(word), &length, *cursor) == -1) {
                set_error(error, error_size, "word is too long");
                return -1;
            }
            began = true;
            cursor++;
            continue;
        }

        if (quote != '\'' && value == '$') {
            if (lex_variable(&cursor, word, sizeof(word), &length) == -1) {
                set_error(error, error_size, "invalid or too long variable");
                return -1;
            }
            began = true;
            continue;
        }

        if (append_character(word, sizeof(word), &length, value) == -1) {
            set_error(error, error_size, "word is too long");
            return -1;
        }
        began = true;
        cursor++;
    }

    if (quote != '\0') {
        set_error(error, error_size, "unclosed quote");
        return -1;
    }
    if (!began) {
        set_error(error, error_size, "unable to parse word");
        return -1;
    }
    if (add_token(parsed, TOKEN_WORD, word, error, error_size) == -1) {
        return -1;
    }

    *input = cursor;
    return 0;
}

static int lex_operator(const char **input, ParsedLine *parsed,
                        char *error, size_t error_size)
{
    const char *cursor = *input;
    TokenKind kind;
    const char *text;

    switch (*cursor) {
    case '|':
        kind = TOKEN_PIPE;
        text = "|";
        cursor++;
        break;
    case '<':
        kind = TOKEN_INPUT;
        text = "<";
        cursor++;
        break;
    case '>':
        if (cursor[1] == '>') {
            kind = TOKEN_APPEND;
            text = ">>";
            cursor += 2;
        } else {
            kind = TOKEN_OUTPUT;
            text = ">";
            cursor++;
        }
        break;
    case '&':
        kind = TOKEN_BACKGROUND;
        text = "&";
        cursor++;
        break;
    default:
        set_error(error, error_size, "unknown operator");
        return -1;
    }

    if (add_token(parsed, kind, text, error, error_size) == -1) {
        return -1;
    }
    *input = cursor;
    return 0;
}

static int lex_line(const char *line, ParsedLine *parsed,
                    char *error, size_t error_size)
{
    const char *cursor = line;

    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        if (is_operator_character(*cursor)) {
            if (lex_operator(&cursor, parsed, error, error_size) == -1) {
                return -1;
            }
        } else {
            if (lex_word(&cursor, parsed, error, error_size) == -1) {
                return -1;
            }
        }
    }

    return 0;
}

static Command *current_command(Pipeline *pipeline)
{
    if (pipeline->command_count == 0) {
        return NULL;
    }
    return &pipeline->commands[pipeline->command_count - 1];
}

static int begin_command(Pipeline *pipeline, char *error, size_t error_size)
{
    if (pipeline->command_count >= SHELL_MAX_COMMANDS) {
        set_error(error, error_size, "too many pipeline stages");
        return -1;
    }

    pipeline->command_count++;
    return 0;
}

static int add_argument(Command *command, char *argument,
                        char *error, size_t error_size)
{
    if (command->argc + 1 >= SHELL_MAX_ARGS) {
        set_error(error, error_size, "too many command arguments");
        return -1;
    }

    command->argv[command->argc++] = argument;
    command->argv[command->argc] = NULL;
    return 0;
}

static int parse_tokens(ParsedLine *parsed, char *error, size_t error_size)
{
    Pipeline *pipeline = &parsed->pipeline;

    if (parsed->token_count == 0) {
        return 0;
    }
    if (begin_command(pipeline, error, error_size) == -1) {
        return -1;
    }

    for (size_t index = 0; index < parsed->token_count; index++) {
        Token *token = &parsed->tokens[index];
        Command *command = current_command(pipeline);

        if (token->kind == TOKEN_WORD) {
            if (add_argument(command, token->text, error, error_size) == -1) {
                return -1;
            }
            continue;
        }

        if (token->kind == TOKEN_PIPE) {
            if (command == NULL || command->argc == 0) {
                set_error(error, error_size, "empty command before pipe");
                return -1;
            }
            if (begin_command(pipeline, error, error_size) == -1) {
                return -1;
            }
            continue;
        }

        if (token->kind == TOKEN_BACKGROUND) {
            if (index + 1 != parsed->token_count) {
                set_error(error, error_size, "& must be the final token");
                return -1;
            }
            pipeline->background = true;
            continue;
        }

        if (index + 1 >= parsed->token_count ||
            parsed->tokens[index + 1].kind != TOKEN_WORD) {
            set_error(error, error_size, "redirection requires a file name");
            return -1;
        }

        char *path = parsed->tokens[++index].text;
        if (token->kind == TOKEN_INPUT) {
            if (command->input_path != NULL) {
                set_error(error, error_size, "multiple input redirections");
                return -1;
            }
            command->input_path = path;
        } else {
            if (command->output_path != NULL) {
                set_error(error, error_size, "multiple output redirections");
                return -1;
            }
            command->output_path = path;
            command->append_output = token->kind == TOKEN_APPEND;
        }
    }

    for (size_t index = 0; index < pipeline->command_count; index++) {
        Command *command = &pipeline->commands[index];
        if (command->argc == 0) {
            set_error(error, error_size, "empty command in pipeline");
            return -1;
        }
        if (index > 0 && command->input_path != NULL) {
            set_error(error, error_size,
                      "input redirection is only valid on the first stage");
            return -1;
        }
        if (index + 1 < pipeline->command_count &&
            command->output_path != NULL) {
            set_error(error, error_size,
                      "output redirection is only valid on the last stage");
            return -1;
        }
    }

    return 0;
}

int shell_parse(const char *line, ParsedLine *parsed, char *error,
                size_t error_size)
{
    if (line == NULL || parsed == NULL) {
        set_error(error, error_size, "invalid parser input");
        return -1;
    }

    memset(parsed, 0, sizeof(*parsed));

    if (lex_line(line, parsed, error, error_size) == -1) {
        shell_free_parsed(parsed);
        return -1;
    }

    if (parse_tokens(parsed, error, error_size) == -1) {
        shell_free_parsed(parsed);
        return -1;
    }

    return 0;
}

void shell_free_parsed(ParsedLine *parsed)
{
    if (parsed == NULL) {
        return;
    }

    for (size_t index = 0; index < parsed->token_count; index++) {
        free(parsed->tokens[index].text);
        parsed->tokens[index].text = NULL;
    }

    parsed->token_count = 0;
    memset(&parsed->pipeline, 0, sizeof(parsed->pipeline));
}
