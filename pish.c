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
/*
 * Prints out the usage error message
 */
void usage_error(void) {
    fprintf(stderr, "pish: Usage error\n");
    fflush(stderr);
}
/*
 * Helper function which trims whitespace from a string
 * @param string    A string to remove the leading and trailing whitespace
 * @return          The parsed string without the leading and trailing
 * whitespace
 */
char *trim_whitespace(char *string) {
    char *end;
    // remove leading space type characters
    while (isspace((unsigned char)*string)) {
        string++;
    }
    // if we are at the end of a string, then we return the string which is just
    // a null-terminator
    if (*string == '\0') {
        return string;
    }
    // remove the trailing space type characters
    end = string + strlen(string) - 1;
    while (end > string && isspace((unsigned char)*end)) {
        end--;
    }
    // null terminate the string
    *(end + 1) = '\0';
    return string;
}
/*
 * Break down a line of input by whitespace, and put the results into
 * a struct pish_arg to be used by other functions.
 *
 * @param command_str   A char buffer containing the input command
 * @param arg           Broken down args will be stored here
 */
void parse_command(char *command_str, struct pish_arg *arg) {
    // get the token before the first tab character from command_str
    char *saveptr;
    char *token = strtok_r(command_str, " \t", &saveptr);
    arg->argc = 0;
    // remove all tab characters
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
 * Run a simple command
 *
 * Built-in commands are handled internally by the pish program.
 * Otherwise, use fork/exec to create child processes to run the program.
 *
 * If the command is empty, it does nothing.
 * If NOT in script mode, adds the command to history file.
 * @param arg       The arg stored as a struct which contains the arg count and
 * the char* array of args
 */
void run(struct pish_arg *arg) {
    // if the arg count is 0, we simply return
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
        // normal exec behavior
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
        // store the last exit status
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
/*
 * This method handles the execution of a subshell
 * @param command_str    The command to be run in the subshell without the
 * beginning and trailing parenthesis
 * @return The exit status of the subshell
 */
int run_subshell(char *command_str) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    } else if (pid == 0) {
        // recursively executes the line inside the subshell
        int subshell_status = execute_chain(command_str);
        exit(subshell_status);
    } else {
        // return the exit status to the parent shell
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
/*
 * The function is responsible for handling pipes between a left and a right command
 * @param left_cmd      The command at the left of the pipe
 * @param right_cmd     The command at the right of the pipe
 * @return              The exit status of the command
 */
int run_pipe(char *left_cmd, char *right_cmd) {
    pid_t left_pid;
    pid_t right_pid;
    int p[2];
    int status;
    //create the pipe
    if (pipe(p) < 0) {
        perror("pipe");
        return 1;
    }
    //handle the left command
    left_pid = fork();
    if (left_pid < 0) {
        perror("fork");
        return 1;
    }
    if (left_pid == 0) {
        // close the read end of the pipe
        close(p[0]);
        // re-direct stdout to the write end of the pipe
        dup2(p[1], STDOUT_FILENO);
        // close the write end file descriptor
        close(p[1]);
        //run the left command but output is instead the write end of the pipe instead of stdout
        int exit_status = execute_chain(left_cmd);
        exit(exit_status);
    }

    right_pid = fork();
    if (right_pid < 0) {
        perror("fork");
        return 1;
    }
    if (right_pid == 0) {
        // close the write end of the pipe
        close(p[1]);
        // re-direct stdin to the read end of the pipe
        dup2(p[0], STDIN_FILENO);

        close(p[0]);
        //run the right command but output is instead the read end of the the pipe instead of stdin
        int exit_status = execute_chain(right_cmd);
        exit(exit_status);
    }
    //close both ends of the pipe in the parent
    close(p[0]);
    close(p[1]);
    //wait for the children to finish
    waitpid(left_pid, NULL, 0);
    waitpid(right_pid, &status, 0);
    //return the exit status
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 1;
}
static char prevDir[MAX_COMMAND_LENGTH];
/*
 * This function is responsible for the execution of a command including the
 * parsing of separation operators
 * @param chain      The command to execute as a string
 * @return           The status of the command which executes
 */
int execute_chain(char *chain) {
    // store the parenthesis level for the subshell
    int paren_level = 0;
    char *scanner = chain + strlen(chain) - 1;
    // parse the chain from right to left to check for pipes
    while (scanner >= chain) {
        if (*scanner == ')') {
            paren_level++;
        } else if (*scanner == '(') {
            paren_level--;
        }
        // only handle pipes which are not inside subshells
        else if (*scanner == '|' && paren_level == 0) {
            // ensure that it is a pipe and not a ||
            if (scanner != chain && *(scanner - 1) == '|') {
                scanner--;
            } else {
                *scanner = '\0';
                return run_pipe(chain, scanner + 1);
            }
        }
        scanner--;
    }

    paren_level = 0;
    scanner = chain;
    // parse the string for subshells and semi-colon separation
    while (*scanner != '\0') {
        if (*scanner == '(') {
            // keep track of the level of parenthesis
            paren_level++;
        } else if (*scanner == ')') {
            // don't want the parenthesis level to be negative
            if (paren_level > 0)
                paren_level--;
        } else if (*scanner == ';' && paren_level == 0) {
            *scanner = '\0';
            // recursively execute the left side and the right side of the
            // semi-colon in the chain
            execute_chain(chain);
            return execute_chain(scanner + 1);
        }
        scanner++;
    }

    paren_level = 0;
    scanner = chain;
    // re-parse the string for parenthesis and && and || separation
    while (*scanner != '\0') {
        if (*scanner == '(') {
            paren_level++;
        } else if (*scanner == ')') {
            if (paren_level > 0)
                paren_level--;
        } else if (paren_level == 0) {
            // upon reaching a &&, execute the left side then execute the right
            // side if the left side has an exit status of 0
            if (strncmp(scanner, "&&", 2) == 0) {
                *scanner = '\0';
                int status = execute_chain(chain);
                if (status == 0) {
                    return execute_chain(scanner + 2);
                } else {
                    return status;
                }
            }
            // upon reaching a ||, execute the left side then execute the right
            // side if the left side has an non-zero exit status
            else if (strncmp(scanner, "||", 2) == 0) {
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
    // keep track of the local exit status
    int local_status = 0;
    char *trimmed_cmd = trim_whitespace(chain);
    // check if the start of the trimmed and parsed command starts with a bang
    // character
    if (strlen(trimmed_cmd) > 0 && *trimmed_cmd == '!') {
        // if it does, handle the command and negate the exit status
        char *post_bang_cmd = trimmed_cmd + 1;
        int status = execute_chain(post_bang_cmd);
        if (status == 0) {
            local_status = 1;
        } else {
            local_status = 0;
        }
    }
    // upon reaching a parenthesis opening, we run the contents inside the
    // parenthesis as a subshell
    else if (*trimmed_cmd == '(') {
        // check it is a valid subshell syntax
        char *end = trimmed_cmd + strlen(trimmed_cmd) - 1;
        if (*end == ')') {
            *end = '\0';
            char *subshell_cmd = trimmed_cmd + 1;
            local_status = run_subshell(subshell_cmd);
        } else {
            fprintf(stderr, "pish: syntax error: missing ')'\n");
            local_status = 2;
        }
    }
    // if the trimmed command is empty, do nothing
    else if (strlen(trimmed_cmd) == 0) {
        local_status = 0;
    }
    // execute the basic command which has been parsed already for all the
    // special characters
    else {
        struct pish_arg *cmd = malloc(sizeof(struct pish_arg));
        if (cmd == NULL) {
            perror("malloc");
            exit(EXIT_FAILURE);
        }
        parse_command(trimmed_cmd, cmd);
        // if the command is empty, do nothing
        if (cmd->argv[0] == NULL) {
            local_status = 0;
        }
        // if the command is cd, change directory
        else if (strcmp(cmd->argv[0], "cd") == 0) {
            if (cmd->argc != 2) {
                usage_error();
                local_status = 1;
            } else {
                char *cwd = getcwd(NULL, 0);
                // handle cd - which changes to the previous directory the shell
                // was in
                if (strcmp(cmd->argv[1], "-") == 0) {
                    if (prevDir[0] == '\0') {
                        printf("%s\n", cwd);
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
                    // store the old directory
                    strcpy(prevDir, cwd);
                    // change the directory
                    if (chdir(cmd->argv[1]) == -1) {
                        perror("cd");
                        local_status = 1;
                    } else {
                        local_status = 0;
                    }
                }
                free(cwd);
            }
        }
        // if the command executed is exit
        else if (strcmp(cmd->argv[0], "exit") == 0) {
            // arg count should be greater than 2
            if (cmd->argc > 2) {
                usage_error();
                local_status = 1;
            }
            // exit with the desired exit code
            else if (cmd->argc == 2) {
                char *endptr;
                long status_val = strtol(cmd->argv[1], &endptr, 10);
                // check that the inputted exit code is a valid number
                if (*endptr != '\0' || endptr == cmd->argv[1]) {
                    fprintf(stderr, "pish: exit: numeric argument required\n");
                    local_status = 2;
                } else {
                    exit((int)status_val & 255);
                }
            } else {
                exit(last_exit_status);
            }
        }
        // if the executed command is history
        else if (strcmp(cmd->argv[0], "history") == 0) {
            // print history if just history
            if (cmd->argc == 1) {
                print_history();
                local_status = 0;
            }
            // clear history if requested
            else if (cmd->argc == 2 && (strcmp(cmd->argv[1], "-c") == 0)) {
                clear_history();
                local_status = 0;
            } else {
                usage_error();
                local_status = 1;
            }
        }
        // handle exec, replacing the shell
        else if (strcmp(cmd->argv[0], "exec") == 0) {
            if (cmd->argc < 2) {
                usage_error();
                local_status = 1;
            } else {
                execvp(cmd->argv[1], cmd->argv + 1);
                perror(cmd->argv[1]);
                exit(127);
            }
        }
        // if it isn't a built-in command, run the command
        else {
            run(cmd);
            local_status = last_exit_status;
        }
        // free created cmd struct
        for (int i = 0; i < cmd->argc; i++) {
            free(cmd->argv[i]);
        }
        free(cmd);
    }

    return local_status;
}
/*
 * This function takes an inputted line and checks if the line should cause a
 * continuation i.e it ends in \ or && or ||
 * @param line      The line to check
 * @return          0 if no continuation should occur, 1 if is a \ character, 2
 * if it is a && or an ||, 3 if it is a pipe 
 */
int check_for_continuation(char *line) {
    int len = strlen(line);
    if (len == 0) {
        return 0;
    }
    if (line[len - 1] == '\\') {
        line[len - 1] = '\0';
        return 1;
    }
    if (len >= 2) {
        if (strcmp(&line[len - 2], "&&") == 0 ||
            strcmp(&line[len - 2], "||") == 0) {
            return 2;
        }
    }
    if(len>=1){
        if(line[len-1]=='|'&&(len==1||line[len-2]!='|')){
            return 3;
        }
    }
    return 0;
}
/*
 * The main loop of pish.
 * @param fp    The input for the shell, stdin or a script
 * @return      Returns the exit status of the shell
 */
int pish(FILE *fp) {
    while (1) {
        // prompt the user if the shell is not in script mode
        if (!script_mode) {
            prompt();
        }
        char *full_command = NULL;
        int continuation_type = 0;
        // do while loop runs until there is no more continuation, it handles
        // the continuation of the commands
        do {
            char line_buffer[MAX_COMMAND_LENGTH];
            // if there is no more lines to run then execute the previously
            // stored command
            if (fgets(line_buffer, MAX_COMMAND_LENGTH, fp) == NULL) {
                // if there exists a previous command, execute it
                if (full_command) {
                    last_exit_status = execute_chain(full_command);
                }
                // if we are not in script mode and the file is stdin, print a
                // new line
                if (!script_mode && isatty(fileno(stdin)))
                    printf("\n");
                return last_exit_status;
            }
            // null-terminate the buffer at the new line character (enter
            // character)
            line_buffer[strcspn(line_buffer, "\n")] = '\0';

            char *trimmed_line = trim_whitespace(line_buffer);
            // if this is the first line of a command then store it in
            // full_command
            if (full_command == NULL) {
                full_command = strdup(trimmed_line);
                if (full_command == NULL) {
                    perror("strdup failed");
                    exit(EXIT_FAILURE);
                }
            }
            // otherwise, handle the successive execution
            else {
                const char *separator;
                // continutation type = 1 means it ends with a \ character,
                // otherwise its a && or a || or a | 
                if (continuation_type == 1) {
                    separator = "";
                } else {
                    separator = " ";
                }
                // need to keep track of lengths to modify full_command size in
                // memory
                size_t old_len = strlen(full_command);
                size_t sep_len = strlen(separator);
                size_t new_part_len = strlen(trimmed_line);
                // allocate more memory for the additional lines
                char *new_full_command =
                    realloc(full_command, old_len + sep_len + new_part_len + 1);
                if (new_full_command == NULL) {
                    perror("realloc failed");
                    exit(EXIT_FAILURE);
                }
                full_command = new_full_command;
                // add the added lines and the corresponding space if it needs
                // one
                strcat(full_command, separator);
                strcat(full_command, trimmed_line);
            }

            continuation_type = check_for_continuation(full_command);
            // print out the continuation message if it isn't in script mode
            if (continuation_type != 0 && !script_mode) {
                printf("> ");
                fflush(stdout);
            }
        } while (continuation_type != 0);
        // if the full command isn't null or empty
        if (full_command && strlen(full_command) > 0) {
            // add the command to history
            if (!script_mode) {
                char *history_copy = strdup(full_command);
                if (history_copy == NULL) {
                    perror("strdup failed for history");
                    exit(EXIT_FAILURE);
                }
                struct pish_arg history_arg;
                parse_command(history_copy, &history_arg);
                if (history_arg.argc > 0) {
                    add_history(&history_arg);
                }
                for (int i = 0; i < history_arg.argc; i++) {
                    free(history_arg.argv[i]);
                }
                free(history_copy);
            }
            // execute the command chain
            char *command_copy = strdup(full_command);
            if (command_copy == NULL) {
                perror("strdup failed");
                exit(EXIT_FAILURE);
            }
            last_exit_status = execute_chain(command_copy);
            free(command_copy);
        } else {
            last_exit_status = 0;
        }
        if (full_command) {
            free(full_command);
            full_command = NULL;
        }
    }
    return last_exit_status;
}

/*
 * The entry point of the pish program.
 * @param argv      Stores the script which the shell should run
 */
int main(int argc, char *argv[]) {
    // if there is no script, assume the input is stdin
    if (argc == 1) {
        pish(stdin);
    }
    // run the shell in script mode if there is a script to run
    else if (argc == 2) {
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
