#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define MAX_CMD_LEN 1024
#define MAX_TOKENS (MAX_CMD_LEN / 2 + 1)
#define PATH_MAX 4096

// Verifica si es builtin
int es_builtin(char *cmd) {
    return strcmp(cmd, "cd") == 0 || strcmp(cmd, "exit") == 0;
}

// Ejecuta builtin
void ejecutar_builtin(char **args) {
    if (strcmp(args[0], "cd") == 0) {
        if (args[1] == NULL) {
            fprintf(stderr, "Uso: cd <directorio>\n");
        } else if (chdir(args[1]) != 0) {
            perror("cd");
        }
    } else if (strcmp(args[0], "exit") == 0) {
        exit(0);
    }
}

// Devuelve ruta si el archivo es ejecutable
char *buscar_en_path(const char *cmd) {
    char *path_env = getenv("PATH");
    if (!path_env) return NULL;

    char *path_copy = strdup(path_env);
    char *saveptr;
    char *dir = strtok_r(path_copy, ":", &saveptr);

    while (dir) {
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);
        if (access(full_path, X_OK) == 0) {
            free(path_copy);
            return strdup(full_path);
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }

    free(path_copy);
    return NULL;
}

// Ejecuta el comando externo (no builtin)
void ejecutar_externo(char **args) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return;
    } else if (pid == 0) {
        // Si existe y se puede ejcutar del tirón
        if (access(args[0], X_OK) == 0) {
            execv(args[0], args);
            perror("execv");
            exit(1);
        }

        // Si no, se busca el path y se ejecuta
        char *path_cmd = buscar_en_path(args[0]);
        if (path_cmd) {
            execv(path_cmd, args);
            perror("execv");
            free(path_cmd);
            exit(1);
        }

        // No encontrado
        fprintf(stderr, "Comando no encontrado: %s\n", args[0]);
        exit(1);
    } else {
        // Espera
        wait(NULL);
    }
}

// Tokeniza línea y ejecuta comando
void ejecutar_linea(char *linea) {
    char *args[MAX_TOKENS];
    int i = 0;
    char *saveptr;
    char *token = strtok_r(linea, " \t\r\n", &saveptr);
    while (token && i < MAX_TOKENS - 1) {
        args[i++] = token;
        token = strtok_r(NULL, " \t\r\n", &saveptr);
    }
    args[i] = NULL;

    // Linea vacia
    if (args[0] == NULL) return;

    if (es_builtin(args[0])) {
        ejecutar_builtin(args);
    } else {
        ejecutar_externo(args);
    }
}

int main() {
    char linea[MAX_CMD_LEN];

    while (1) {
        printf("> ");
        fflush(stdout);

        if (!fgets(linea, sizeof(linea), stdin)) {
            // EOF o error
            break;
        }

        ejecutar_linea(linea);
    }

    printf("\n");
    return 0;
}
