/*
 * hy345sh - a small unix shell
 *
 * HY345 - Leitourgika Systimata, Assignment 1
 * Name: Papadakis Ioannis Titos
 * AM: csd5200
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <ctype.h>
#include <pwd.h>

#define INPUT_SIZE 500
#define MAX_TOKENS 200
#define MAX_STAGES 20
#define MAX_ARGS 50

// #define dbg

char* am;
char* user;

// Types of tokens produced by the lexer. WORD holds actual text (command
// names, arguments, filenames, variable names...), the rest are just the
// special shell symbols.
typedef enum {
    WORD,
    PIPE,
    SEMI,
    REDIR_IN,
    REDIR_OUT,
    REDIR_APPEND
} TokenType;

typedef struct {
    TokenType type;
    char text[INPUT_SIZE];
} Token;

void init_promt()
{
    am = "csd5200";

    // getlogin() needs a real controlling terminal, so it returns NULL
    // when stdin comes from a pipe/file (e.g. testing with ./hy345sh <
    // commands.txt). Fall back to the environment and then to the
    // password database so the prompt still shows a real username.
    user = getlogin();
    if (user == NULL)
        user = getenv("USER");
    if (user == NULL)
    {
        struct passwd* pw = getpwuid(getuid());
        if (pw != NULL)
            user = pw->pw_name;
    }
    if (user == NULL)
        user = "unknown";
}

void print_promt()
{
    char* cwd = getcwd(NULL, 0);
    printf("%s-hy345sh@%s:%s ", am, user, cwd);
    free(cwd);
}

/*
 * Turns the raw input line into a list of tokens, taking care of the shell
 * special characters ; | < >. Quotes (' or ") are used to "protect" text:
 * whatever is inside quotes is copied as is, so things like x="5|5;10" are
 * kept together as one word instead of being split on the ; and |.
 */
int tokenize(char* line, Token tokens[])
{
    int count = 0;
    int len = strlen(line);
    int i = 0;

    while (i < len)
    {
        while (i < len && isspace((unsigned char)line[i]))
            i++;

        if (i >= len)
            break;

        if (line[i] == '|')
        {
            tokens[count].type = PIPE;
            count++;
            i++;
            continue;
        }

        if (line[i] == ';')
        {
            tokens[count].type = SEMI;
            count++;
            i++;
            continue;
        }

        if (line[i] == '<')
        {
            tokens[count].type = REDIR_IN;
            count++;
            i++;
            continue;
        }

        if (line[i] == '>')
        {
            if (i + 1 < len && line[i + 1] == '>')
            {
                tokens[count].type = REDIR_APPEND;
                i += 2;
            }
            else
            {
                tokens[count].type = REDIR_OUT;
                i += 1;
            }
            count++;
            continue;
        }

        // Anything else starts a word. Keep reading characters until we
        // hit whitespace or one of the special symbols above (but only
        // when we are NOT inside quotes).
        char word[INPUT_SIZE];
        int wi = 0;

        while (i < len && !isspace((unsigned char)line[i]) &&
               line[i] != '|' && line[i] != ';' &&
               line[i] != '<' && line[i] != '>')
        {
            if (line[i] == '"' || line[i] == '\'')
            {
                char quote = line[i];
                i++;
                while (i < len && line[i] != quote)
                {
                    word[wi++] = line[i];
                    i++;
                }
                if (i < len)
                    i++; // skip the closing quote
            }
            else
            {
                word[wi++] = line[i];
                i++;
            }
        }

        word[wi] = '\0';
        tokens[count].type = WORD;
        strcpy(tokens[count].text, word);
        count++;
    }

    return count;
}

/*
 * Replaces every $name found in "in" with the value of the environment
 * variable "name" (getenv), and writes the result to "out". If the
 * variable does not exist it is simply replaced with nothing, same as a
 * real shell would do.
 */
void expand_variables(const char* in, char* out)
{
    int len = strlen(in);
    int oi = 0;
    int i = 0;

    while (i < len)
    {
        if (in[i] == '$' && (isalpha((unsigned char)in[i + 1]) || in[i + 1] == '_'))
        {
            int j = i + 1;
            char name[INPUT_SIZE];
            int ni = 0;

            while (j < len && (isalnum((unsigned char)in[j]) || in[j] == '_'))
            {
                name[ni++] = in[j];
                j++;
            }
            name[ni] = '\0';

            char* value = getenv(name);
            if (value != NULL)
            {
                strcpy(&out[oi], value);
                oi += strlen(value);
            }

            i = j;
        }
        else
        {
            out[oi++] = in[i];
            i++;
        }
    }

    out[oi] = '\0';
}

// Checks if "text" has the form name=value (name being a valid identifier).
// On success, fills name/value and returns 1.
int parse_assignment(const char* text, char* name, char* value)
{
    int i = 0;

    if (!(isalpha((unsigned char)text[0]) || text[0] == '_'))
        return 0;

    while (isalnum((unsigned char)text[i]) || text[i] == '_')
        i++;

    if (text[i] != '=')
        return 0;

    strncpy(name, text, i);
    name[i] = '\0';
    strcpy(value, text + i + 1);

    return 1;
}

/*
 * Runs one ';'-separated command, which can itself be a pipeline of
 * several commands connected with '|' and can have <, > or >>
 * redirections on any of its stages.
 */
void execute_segment(Token* seg, int seg_count)
{
    int stage_argc[MAX_STAGES];
    char stage_argv_buf[MAX_STAGES][MAX_ARGS][INPUT_SIZE];
    char* stage_argv[MAX_STAGES][MAX_ARGS + 1];
    char stage_infile[MAX_STAGES][INPUT_SIZE];
    int stage_has_infile[MAX_STAGES];
    char stage_outfile[MAX_STAGES][INPUT_SIZE];
    int stage_has_outfile[MAX_STAGES];
    int stage_append[MAX_STAGES];

    int stage = 0;
    stage_argc[0] = 0;
    stage_has_infile[0] = 0;
    stage_has_outfile[0] = 0;

    for (int i = 0; i < seg_count; i++)
    {
        Token* t = &seg[i];

        if (t->type == PIPE)
        {
            stage++;
            stage_argc[stage] = 0;
            stage_has_infile[stage] = 0;
            stage_has_outfile[stage] = 0;
            continue;
        }

        if (t->type == REDIR_IN)
        {
            i++;
            expand_variables(seg[i].text, stage_infile[stage]);
            stage_has_infile[stage] = 1;
            continue;
        }

        if (t->type == REDIR_OUT || t->type == REDIR_APPEND)
        {
            stage_append[stage] = (t->type == REDIR_APPEND);
            i++;
            expand_variables(seg[i].text, stage_outfile[stage]);
            stage_has_outfile[stage] = 1;
            continue;
        }

        // plain WORD -> one more argv entry for the current stage
        int argi = stage_argc[stage];
        expand_variables(t->text, stage_argv_buf[stage][argi]);
        stage_argv[stage][argi] = stage_argv_buf[stage][argi];
        stage_argc[stage]++;
    }

    int nstages = stage + 1;

    for (int s = 0; s < nstages; s++)
        stage_argv[s][stage_argc[s]] = NULL;

    if (stage_argc[0] == 0 && nstages == 1)
        return; // empty line, nothing to do

    int pipefd[MAX_STAGES][2];
    for (int s = 0; s < nstages - 1; s++)
    {
        if (pipe(pipefd[s]) == -1)
        {
            perror("pipe");
            return;
        }
    }

    pid_t pids[MAX_STAGES];

    for (int s = 0; s < nstages; s++)
    {
        pid_t pid = fork();

        if (pid == 0) // child
        {
            if (s > 0)
                dup2(pipefd[s - 1][0], STDIN_FILENO);
            if (s < nstages - 1)
                dup2(pipefd[s][1], STDOUT_FILENO);

            for (int k = 0; k < nstages - 1; k++)
            {
                close(pipefd[k][0]);
                close(pipefd[k][1]);
            }

            if (stage_has_infile[s])
            {
                int fd = open(stage_infile[s], O_RDONLY);
                if (fd == -1)
                {
                    perror(stage_infile[s]);
                    exit(EXIT_FAILURE);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            if (stage_has_outfile[s])
            {
                int flags = O_WRONLY | O_CREAT | (stage_append[s] ? O_APPEND : O_TRUNC);
                int fd = open(stage_outfile[s], flags, 0644);
                if (fd == -1)
                {
                    perror(stage_outfile[s]);
                    exit(EXIT_FAILURE);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            execvp(stage_argv[s][0], stage_argv[s]);
            perror("execvp failed");
            exit(EXIT_FAILURE);
        }
        else if (pid > 0) // parent
        {
            pids[s] = pid;
        }
        else
        {
            printf("Fork not executed successfully!\n");
        }
    }

    for (int s = 0; s < nstages - 1; s++)
    {
        close(pipefd[s][0]);
        close(pipefd[s][1]);
    }

    for (int s = 0; s < nstages; s++)
    {
        int status;
        waitpid(pids[s], &status, 0);
    }
}

void read_promt()
{
    char line[INPUT_SIZE];

    if (fgets(line, INPUT_SIZE, stdin) == NULL)
    {
        printf("\n");
        exit(EXIT_SUCCESS);
    }

    line[strcspn(line, "\n")] = '\0';

    Token tokens[MAX_TOKENS];
    int count = tokenize(line, tokens);

#ifdef dbg
    for (int i = 0; i < count; i++)
        printf("token %d: type=%d text=%s\n", i, tokens[i].type, tokens[i].text);
#endif

    // Split the tokens on ';' and handle each command one at a time, in
    // order (so e.g. "cat y1;rm y1" waits for cat to finish before rm
    // runs).
    int seg_start = 0;
    for (int i = 0; i <= count; i++)
    {
        if (i == count || tokens[i].type == SEMI)
        {
            int seg_count = i - seg_start;

            if (seg_count > 0)
            {
                char name[INPUT_SIZE];
                char value[INPUT_SIZE];

                if (seg_count == 1 && tokens[seg_start].type == WORD &&
                    strcmp(tokens[seg_start].text, "exit") == 0)
                {
                    exit(EXIT_SUCCESS);
                }
                else if (seg_count == 1 && tokens[seg_start].type == WORD &&
                         parse_assignment(tokens[seg_start].text, name, value))
                {
                    char expanded[INPUT_SIZE];
                    expand_variables(value, expanded);
                    setenv(name, expanded, 1);
                }
                else
                {
                    execute_segment(&tokens[seg_start], seg_count);
                }
            }

            seg_start = i + 1;
        }
    }
}

int main(void)
{
    init_promt();

    while (1)
    {
        print_promt();
        read_promt();
    }

    return 0;
}
