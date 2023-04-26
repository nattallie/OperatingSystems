#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;


/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int cmd_exit(struct tokens *tokens);
int cmd_help(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);
int cmd_wait(struct tokens *tokens);
int cmd_prog(struct tokens *tokens);
char** make_args(struct tokens *tokens);
int try_path_perm(char** args, char* path);
void sigtstp_handler(int signo);


/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
  {cmd_help, "?", "show this help menu"},
  {cmd_exit, "exit", "exit the command shell"},
  {cmd_pwd, "pwd", "print current directory"},
  {cmd_cd, "cd", "change current directory"},
  {cmd_wait, "wait", "wait all processes in background"}
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens) {
  exit(0);
}

/* Prints current directory */
int cmd_pwd(unused struct tokens *tokens) {
  char dir[4096];
  if (getcwd(dir, sizeof(dir)) != NULL) {
    printf("%s\n", dir);
  } else {
    fprintf(stderr, "%s\n", strerror(errno));
    return -1;
  }
  return 0;
}

/* Change current directory */
int cmd_cd(struct tokens *tokens) {
  if (tokens_get_length(tokens) < 2) {
    fprintf(stderr, "%s\n", "Not enough arguments for cd");
    return -1;
  } else {
    int suc = chdir(tokens_get_token(tokens, 1));
    if (suc == -1) {
      fprintf(stderr, "%s\n", strerror(errno));
      return -1;
    }
  }
  return 0;
}

/* This takes care of waiting background processes 
  to complete their jobs */
int cmd_wait(struct tokens *tokens) {
  waitpid(-1, NULL, 0);
  return 0;
}


/* This function is intentionally left empty. Shell
  should ignore itself this signal, but it should alert
  child process. */
void sigtstp_handler(int signo) {
}


/* Trying to get all possible path and looks for desirable filename */
int try_path_perm(char** args, char* path) {
  DIR* cdir = NULL;
  struct dirent *pDirent;
  cdir = opendir(path);

  if (cdir == NULL) {
    return -1;
  }

  while ((pDirent = readdir(cdir)) != NULL) {
    if (strcmp(".", pDirent->d_name) != 0 && strcmp("..", pDirent->d_name) != 0) {
      char* new_path = malloc(sizeof(char) * (strlen(path) + strlen(pDirent->d_name)) + 2);

      if (new_path != NULL) {
        new_path[0] = '\0';
        strcat(new_path, path);
        strcat(new_path, "/");
        strcat(new_path, pDirent->d_name);
        if (strcmp(pDirent->d_name, args[0]) == 0) {
          args[0] = new_path;
          closedir(cdir);
          return 1;
        }

        if (try_path_perm(args, new_path) == 1) {
          closedir(cdir);
          free(new_path);
          return 1;
        }
        free(new_path);
      } else {
          fprintf(stderr, "%s\n", "Free space problem");
      }

    }
  }
  closedir(cdir);
  return -1;
}

/* It considers all possible path and changes first argument */
int fill_path_arg(char** args) {
  int ind = -1;
  char* env_path = getenv("PATH");
  char* full_path = malloc(sizeof(char) * strlen(env_path) + 1);
  full_path = strcpy(full_path, env_path);

  if (full_path == NULL) {
    fprintf(stderr, "%s\n", "free space problem");
    return -1;
  }

  full_path = strtok(full_path, ":");
  while (full_path) {
    if (ind == -1) {
      ind = try_path_perm(args, full_path);
    }      
    full_path = strtok(NULL, ":");
  }
  return ind;
}

/* Executes program which is first argument of token.
   If this word begins with "/" then it considers arg as full path.
   Otherwise it considers all possible path and then executes
   child process. */
int cmd_prog(struct tokens *tokens) {
  int pid = fork();
  if (pid < 0) {
    fprintf(stderr, "%s\n", "Error occured while forking child process\n");
    return -1;
  }

  if (pid == 0) {
    char** args = (char**) make_args(tokens);
    if (args == NULL) exit(-1);

    int ind;
    if (*(args[0]) == '.') {
      char* tmp = malloc(sizeof(char) * 196);
      tmp = getcwd(tmp, 196);
      tmp = strcat(tmp, "/");
      tmp = strcat(tmp, args[0] + 2);
      args[0] = tmp;
      ind = 1;
    } else if (*(args[0]) != '/') {
      ind = fill_path_arg(args);
    } else {
      ind = 1;
    }

    if (ind == -1) {
      exit(errno);
    }

    setpgid(getpid(), getpid());

    execv(args[0], args);
    exit(errno);
  } 

  if (tokens_get_length(tokens) > 1) {
    char* last_tok = tokens_get_token(tokens, tokens_get_length(tokens) - 1);
    if (strcmp(last_tok, "&") == 0) {
      tcsetpgrp(STDIN_FILENO, getpid());
      return 0;
    }
  }

  int stat = -1;
  if (tcsetpgrp(STDIN_FILENO, pid) == -1) {
    fprintf(stderr, "%s\n", strerror(errno));
  }

  waitpid(pid, &stat, WUNTRACED);

  if (WIFEXITED(stat) &&  WEXITSTATUS(stat) != 0) {
    fprintf(stderr, "%s\n", strerror(WEXITSTATUS(stat)));
  }
  
  if (tcsetpgrp(STDIN_FILENO, getpid()) == -1) {
    fprintf(stderr, "%s\n", strerror(errno));
  }

// /vagrant/General/Freeuni/OS/individual-assignements/hw1/shell
  return 0;
}

/* Simply creates array of char* from tokens.
   It also considers '<' and  '>' signs. */ 
char** make_args(struct tokens *tokens) {
  size_t len = tokens_get_length(tokens);
  char** buf = malloc(sizeof(char*) * len + 1);
  int cnt = 0;
  int left = 0;
  int right = 0;

  for (int i = 0; i < len; i++) {
    char* cur_tok = tokens_get_token(tokens, i);
    if (left) {
      left = 0;
      int old_fd = open(cur_tok, O_RDONLY);
      if (old_fd == -1 || dup2(old_fd, STDIN_FILENO) == -1 || close(old_fd) == -1) {
        fprintf(stderr, "%s\n", "Problem while dupping");
        return NULL;
      }
      continue;
    }

    if (right) {
      right = 0;
      int old_fd2 = creat(cur_tok, S_IREAD|S_IWRITE|S_IRGRP|S_IWGRP|S_IROTH);
      if (old_fd2 == -1 || dup2(old_fd2, STDOUT_FILENO) == -1 || close(old_fd2) == -1) {
        fprintf(stderr, "%s\n", "Problem while dupping");
        return NULL;
      }
      continue;
    }

    if (strcmp(cur_tok, "&") == 0) {
      continue;
    }
    
    if (strcmp(cur_tok, "<") == 0) {
      left = 1;
    } else if (strcmp(cur_tok, ">") == 0) {
      right = 1;
    } else {
      buf[cnt] = tokens_get_token(tokens, i);
      cnt++;
    }
  }

  buf[cnt] = NULL;
  return buf;
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (!shell_is_interactive) {
    fprintf(stderr, "%s\n", strerror(errno));
  }

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */

    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();
    
    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

int main(unused int argc, unused char *argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  signal(SIGTSTP, sigtstp_handler);
  signal(SIGTTOU, SIG_IGN);
  signal(SIGINT, SIG_IGN);

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      cmd_prog(tokens);
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
