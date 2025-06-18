#include <ctype.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wait.h>

#include "pish_history.h"
#define MAX_COMMAND_LENGTH 256

/*
 * Script mode flag. If set to 0, the shell reads from stdin. If set to 1,
 * the shell reads from a file from argv[1].
 */
static int script_mode = 0;
static int last_exit_status = 0;

/*
 * Prints a prompt IF NOT in script mode (see script_mode global flag).
 */
void prompt(void) {
    if (!script_mode) {
        char *working_dir = getcwd(NULL, 0);
        struct passwd *user = getpwuid(getuid());
#ifdef PISH_AUTOGRADER
        printf("%s@pish %s$\n", user->pw_name, working_dir);
#else
        printf("\e[0;35m%s@pish \e[0;34m%s\e[0m$ ", user->pw_name, working_dir);
#endif
        fflush(stdout);
        free(working_dir);
    }
}

void usage_error(void) {
    fprintf(stderr, "pish: Usage error\n");
    fflush(stderr);
}
char *trim_whitespace(char *string) {
    char *end;
    while (isspace((unsigned char)*string)) {
        string++;
    }
    if (*string == '\0') {
        return string;
    }
    end = string + strlen(string) - 1;
    while (end > string && isspace((unsigned char)*end)) {
        end--;
    }
    *(end + 1) = '\0';
    return string;
}
/*
 * Break down a line of input by whitespace, and put the results into
 * a struct pish_arg to be used by other functions.
 *
 * @param command   A char buffer containing the input command
 * @param arg       Broken down args will be stored here
 */
void parse_command(char *command_str, struct pish_arg *arg) {
    char *saveptr;
    char *token = strtok_r(command_str, " \t", &saveptr);
    arg->argc = 0;

    while (token != NULL) {
        char *argument = malloc(strlen(token) + 1);
        if (argument == NULL) {
            perror("malloc failed in parse_command");
            exit(EXIT_FAILURE);
        }
        strcpy(argument, token);
        arg->argv[arg->argc] = argument;
        arg->argc++;

        token = strtok_r(NULL, " \t", &saveptr);
    }
    arg->argv[arg->argc] = NULL; // Null-terminate the argv array
}

/*
 * Run a command.
 *
 * Built-in commands are handled internally by the pish program.
 * Otherwise, use fork/exec to create child processes to run the program.
 *
 * If the command is empty, do nothing.
 * If NOT in script mode, add the command to history file.
 */
void run(struct pish_arg *arg) {
    if (arg->argc == 0) {
        last_exit_status = 0;
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        last_exit_status = 1;
        return;
    } else if (pid == 0) {
        execvp(arg->argv[0], arg->argv);
        perror(arg->argv[0]);
        exit(127);
    } else {
        int status;
        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
            last_exit_status = 1;
            return;
        }
        if (WIFEXITED(status)) {
            last_exit_status = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            last_exit_status = 128 + WTERMSIG(status);
        } else {
            last_exit_status = 1;
        }
    }
}
int execute_chain(char *chain);
int run_subshell(char *command_str) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    } else if (pid == 0) {
        int subshell_status = execute_chain(command_str);
        exit(subshell_status);
    } else {
        int status;
        if (waitpid(pid, &status, 0) == -1) {
            perror("waitpid");
            return 1;
        }
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            return 128 + WTERMSIG(status);
        } else {
            return 1;
        }
    }
}
static char prevDir[MAX_COMMAND_LENGTH];
int execute_chain(char *chain) {
    int paren_level = 0;
    char *scanner = chain;
    while (*scanner != '\0') {
        if (*scanner == '(') {
            paren_level++;
        } else if (*scanner == ')') {
            if (paren_level > 0)
                paren_level--;
        } else if (*scanner == ';' && paren_level == 0) {
            *scanner = '\0';
            execute_chain(chain);
            return execute_chain(scanner + 1);
        }
        scanner++;
    }

    paren_level = 0;
    scanner = chain;
    while (*scanner != '\0') {
        if (*scanner == '(') {
            paren_level++;
        } else if (*scanner == ')') {
            if (paren_level > 0)
                paren_level--;
        } else if (paren_level == 0) {
            if (strncmp(scanner, "&&", 2) == 0) {
                *scanner = '\0';
                int status = execute_chain(chain);
                if (status == 0) {
                    return execute_chain(scanner + 2);
                } else {
                    return status;
                }
            } else if (strncmp(scanner, "||", 2) == 0) {
                *scanner = '\0';
                int status = execute_chain(chain);
                if (status != 0) {
                    return execute_chain(scanner + 2);
                } else {
                    return status;
                }
            }
        }
        scanner++;
    }

    int local_status = 0;
    char *trimmed_cmd = trim_whitespace(chain);
    if (strlen(trimmed_cmd)>0&&*trimmed_cmd == '!') {
        char *post_bang_cmd = trimmed_cmd + 1;
        int status = execute_chain(post_bang_cmd);
        if (status == 0) {
            local_status = 1;
        } else {
            local_status = 0;
        }
    }
    else if (*trimmed_cmd == '(') {
        char *end = trimmed_cmd + strlen(trimmed_cmd) - 1;
        if (*end == ')') {
            *end = '\0';
            char *subshell_cmd = trimmed_cmd + 1;
            local_status = run_subshell(subshell_cmd);
        } else {
            fprintf(stderr, "pish: syntax error: missing ')'\n");
            local_status = 2;
        }
    } else if (strlen(trimmed_cmd) == 0) {
        local_status = 0;
    } else {
        struct pish_arg *cmd = malloc(sizeof(struct pish_arg));
        if (cmd == NULL) {
            perror("malloc");
            exit(EXIT_FAILURE);
        }
        parse_command(trimmed_cmd, cmd);

        if (cmd->argv[0] == NULL) {
            local_status = 0;
        } else if (strcmp(cmd->argv[0], "cd") == 0) {
            if (cmd->argc != 2) {
                usage_error();
                local_status = 1;
            } else {
                char *cwd = getcwd(NULL, 0);
                if (strcmp(cmd->argv[1], "-") == 0) {
                    if (prevDir[0] == '\0') {
                        fprintf(stderr, "pish: cd: OLDPWD not set\n");
                        local_status = 1;
                    } else {
                        char temp[MAX_COMMAND_LENGTH];
                        strcpy(temp, prevDir);
                        strcpy(prevDir, cwd);
                        if (chdir(temp) == -1) {
                            perror("cd");
                            local_status = 1;
                            strcpy(prevDir, temp);
                        } else {
                            local_status = 0;
                            char *new_cwd = getcwd(NULL, 0);
                            printf("%s\n", new_cwd);
                            free(new_cwd);
                        }
                    }
                } else {
                    strcpy(prevDir, cwd);
                    if (chdir(cmd->argv[1]) == -1) {
                        perror("cd");
                        local_status = 1;
                    } else {
                        local_status = 0;
                    }
                }
                free(cwd);
            }
        } else if (strcmp(cmd->argv[0], "exit") == 0) {
            if (cmd->argc > 2) {
                usage_error();
                local_status = 1;
            } else if (cmd->argc == 2) {
                char *endptr;
                long status_val = strtol(cmd->argv[1], &endptr, 10);
                if (*endptr != '\0' || endptr == cmd->argv[1]) {
                    fprintf(stderr, "pish: exit: numeric argument required\n");
                    local_status = 2;
                } else {
                    exit((int)status_val & 255);
                }
            } else {
                exit(last_exit_status);
            }
        } else if (strcmp(cmd->argv[0], "history") == 0) {
            if (cmd->argc == 1) {
                print_history();
                local_status = 0;
            } else if (cmd->argc == 2 && (strcmp(cmd->argv[1], "-c") == 0)) {
                clear_history();
                local_status = 0;
            } else {
                usage_error();
                local_status = 1;
            }
        } else if (strcmp(cmd->argv[0], "exec") == 0) {
            if (cmd->argc < 2) {
                usage_error();
                local_status = 1;
            } else {
                execvp(cmd->argv[1], cmd->argv + 1);
                perror(cmd->argv[1]);
                exit(127);
            }
        } else {
            run(cmd);
            local_status = last_exit_status;
        }

        if (script_mode == 0 && cmd->argc != 0) {
        }
        for (int i = 0; i < cmd->argc; i++) {
            free(cmd->argv[i]);
        }
        free(cmd);
    }

    return local_status;
}
/*
 * The main loop of pish. Repeat until the "exit" command or EOF:
 * 1. Print the prompt
 * 2. Read command from fp (which can be stdin or a script file)
 * 3. Execute the command
 *
 * Assume that each command never exceeds MAX_COMMAND_LENGTH-1 chars.
 */
int pish(FILE *fp) {
    char full_line[MAX_COMMAND_LENGTH];
    if (script_mode == 0) {
        prompt();
    }
    while (fgets(full_line, MAX_COMMAND_LENGTH, fp) != NULL) {
        if (strlen(full_line) > 0 && full_line[strlen(full_line) - 1] == '\n') {
            full_line[strlen(full_line) - 1] = '\0';
        }

        char *command = trim_whitespace(full_line);
        if (strlen(command) > 0) {
            char *command_copy = strdup(command);
            if (command_copy == NULL) {
                perror("strdup failed");
                exit(EXIT_FAILURE);
            }
            last_exit_status = execute_chain(command_copy);
            free(command_copy);
        }

        if (script_mode == 0) {
            prompt();
        }
    }
    return 0;
}

/*
 * The entry point of the pish program.
 *
 * - If the program is called with no additional arguments (like "./pish"),
 *   process commands from stdin.
 * - If the program is called with one additional argument
 *   (like "./pish script.sh"), process commands from the file specified by the
 *   additional argument under script mode.
 * - If there are more arguments, call usage_error() and exit with status 1.
 */
int main(int argc, char *argv[]) {
    if (argc == 1) {
        pish(stdin);
    } else if (argc == 2) {
        script_mode = 1;
        FILE *fp = fopen(argv[1], "r");
        if (fp == NULL) {
            perror(argv[1]);
            return EXIT_FAILURE;
        }
        pish(fp);
    } else {
        usage_error();
        exit(1);
    }
    return EXIT_SUCCESS;
}
