#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "pish_history.h"

static char pish_history_path[1024] = {'\0'};

/*
 * Set history file path to ~/.pish_history.
 */
static void set_history_path() {
    const char *home = getpwuid(getuid())->pw_dir;
    strncpy(pish_history_path, home, 1024);
    strcat(pish_history_path, "/.pish_history");
}

/*
 * Append the command represented by the given struct pish_arg to the history
 * file at pish_history_path. Separate argv values using a single space.
 */
void add_history(const struct pish_arg *arg) {
    if (!(*pish_history_path)) {
        set_history_path();
    }
    FILE *history = fopen(pish_history_path,"a");
    if(history==NULL){
        perror(pish_history_path);
        return;
    }
    char buffer[256];
    buffer[0]='\0';
    int i = 0;
    if(arg->argv[i]!=0){
        strcat(buffer,arg->argv[i]);
        i++;
    }
    while(arg->argv[i]!=0){
        strcat(buffer," ");
        strcat(buffer,arg->argv[i]);
        i++;
    }
    fprintf(history,"%s\n",buffer);
    fclose(history);
}

/*
 * Print the contents of the file at pish_history_path with line numbers.
 * Each line of output should consist of the line number, a space, and the
 * line itself.
 *
 * For example, if the history file contains:
 * echo Hello 1
 * pwd
 *
 * Then, this function should print:
 * 1 echo Hello 1
 * 2 pwd
 */
void print_history() {
    if (!(*pish_history_path)) {
        set_history_path();
    }
    FILE *history = fopen(pish_history_path,"r");
    char line[256];
    int count = 1;
    while(fgets(line,256,history)!=NULL){
        printf("%d %s",count,line);
        count++;
    }
    fclose(history);
}

/*
 * Clear the contents of the file at pish_history_path.
 */
void clear_history() {
    if (!(*pish_history_path)) {
        set_history_path();
    }
    FILE *history = fopen(pish_history_path,"w");
    if(history==NULL){
        perror(pish_history_path);
        return;
    }
    fclose(history);
}
