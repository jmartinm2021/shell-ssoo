#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define MAX_CMD_LEN 1024
#define MAX_TOKENS (MAX_CMD_LEN / 2 + 1)
#define PATH_MAX 4096

// Verifica si es builtin
int es_builtin(char *cmd) {
    return strcmp(cmd, "cd") == 0 || strcmp(cmd, "exit") == 0;
}

// Reemplazar variables de entorno
int reemplazar_variables(char **args) {
    for (int i = 0; args[i] != NULL; i++) {
        char *token = args[i];
        // Ver si el token empieza $ (variable de entorno)
        if (token[0] == '$') {
            // Nombre de la variable de entorno 
            char *var_name = token + 1;
            // Obtiene el valor
            char *value = getenv(var_name);

            if (value) {
                args[i] = value;
            } else {
                fprintf(stderr, "error: var %s does not exist\n", var_name);
                return 0;
            }
        }
    }
    return 1;
}

// Ejecuta builtin
void ejecutar_builtin(char **args) {
    if (strcmp(args[0], "cd") == 0) {
        if (args[1] == NULL) {
            char *home = getenv("HOME");
            if (home == NULL) {
                fprintf(stderr, "No se pudo determinar el directorio HOME.\n");
                return;
            }
            if (chdir(home) != 0) {
                perror("cd");
            }
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
void ejecutar_externo(char **args, int fd_in, int fd_out, char *in_file, char *out_file, int bg) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return;
    } else if (pid == 0) {
        if (fd_in != -1) {
            if (dup2(fd_in, STDIN_FILENO) == -1) {
                perror("dup2 (entrada)");
                exit(1);
            }
            close(fd_in);
        } else if (bg) {
            // Si está en segundo plano y no se redirigió entrada, se usa /dev/null
            int dev_null = open("/dev/null", O_RDONLY);
            if (dev_null == -1) {
                perror("open /dev/null");
                exit(1);
            }
            if (dup2(dev_null, STDIN_FILENO) == -1) {
                perror("dup2 /dev/null");
                exit(1);
            }
            close(dev_null);
        }
        if (fd_out != -1) {
            if (dup2(fd_out, STDOUT_FILENO) == -1) {
                perror("dup2 (salida)");
                exit(1);
            }
            close(fd_out);
        }
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
        if (!bg) {
            // Espera
            waitpid(pid, NULL, 0);
        } else {
            printf("[ejecutando en segundo plano] PID: %d\n", pid);
        }
    }
}

// Tokeniza línea y ejecuta comando
void ejecutar_linea(char *linea) {
    char *args[MAX_TOKENS];
    int i = 0;
    char *saveptr;
    char *token = strtok_r(linea, " \t\r\n", &saveptr);
    char *in_file = NULL;
    char *out_file = NULL;

    int fd_in = -1;
    int fd_out = -1;
    int bg = 0;

    while (token && i < MAX_TOKENS - 1) {
        // Si hay redireccion de entrada y hay siguiente token se intenta abrir como lectura, si hay de salida se abre para escritura
        if (strcmp(token, "<") == 0 && (token = strtok_r(NULL, " \t\r\n", &saveptr))) {
            fd_in = open(token, O_RDONLY);
            // Error al abrir
            if (fd_in < 0) {
                perror("open");
                return;
            }
            in_file = token;
        } else if (strcmp(token, ">") == 0 && (token = strtok_r(NULL, " \t\r\n", &saveptr))) {
            fd_out = open(token, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            // Error al abrir
            if (fd_out < 0) {
                perror("open");
                return;
            }
            out_file = token;
        } else if (strcmp(token, "&") == 0) {
            // Background flag
            bg = 1;
        } else if (strchr(token, '=') != NULL) {
            // eq_pos apunta a la posición del = en el token
            char *eq_pos = strchr(token, '=');
            // eq_pos ahora es un caracter nulo y token tiene la primera parte (x=y; token = x)
            *eq_pos = '\0';
            // Se le asigna a token el valor de lo que hay despues del eq_pos (ahora nada, antes el =), si ya existia se sobreescribe
            setenv(token, eq_pos + 1, 1);
        } else {
            args[i++] = token;
        }
        token = strtok_r(NULL, " \t\r\n", &saveptr);
    }
    args[i] = NULL;

    // Linea vacia
    if (args[0] == NULL) return;

    // Solo continua la ejecucion si se consigue asignar la variable de entorno
    if (!reemplazar_variables(args)) {
        return;
    }

    if (es_builtin(args[0])) {
        ejecutar_builtin(args);
    } else {
        ejecutar_externo(args, fd_in, fd_out, in_file, out_file, bg);
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
