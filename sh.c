#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <glob.h>
#include <signal.h>
#include <errno.h>

#define MAX_CMD_LEN 1024
#define MAX_TOKENS (MAX_CMD_LEN / 2 + 1)
#define PATH_MAX 4096

int last_result = 0;

// Almacenar pid de procesos en segundo plano
pid_t background_pids[MAX_CMD_LEN];
int bg_pid_count = 0;

// Contador de forks
int fork_counter = 0;

// Historial
int hist_fd = -1;
FILE *hist_file = NULL;

// Verifica si es builtin
int
es_builtin(char *cmd)
{
	return strcmp(cmd, "cd") == 0 || strcmp(cmd, "exit") == 0
	    || strcmp(cmd, "pidsbg") == 0 || strcmp(cmd, "nforks") == 0;
}

// Reemplazar variables de entorno
int
reemplazar_variables(char **args)
{
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
				fprintf(stderr,
					"error: var %s does not exist\n",
					var_name);
				return 0;
			}
		}
	}
	return 1;
}

// Globbing
int
expandir_globbing(char **args)
{
	for (int i = 0; args[i] != NULL; i++) {
		// Verifica si el token contiene algún glob (*  ?  [])
		if (strpbrk(args[i], "*?[]") != NULL) {
			glob_t glob_result;

			glob(args[i], 0, NULL, &glob_result);

			if (glob_result.gl_pathc > 0) {
				// Si hay coincidencias, reemplaza el argumento con los archivos encontrados
				args[i] = strdup(glob_result.gl_pathv[0]);
				for (int j = 1; j < glob_result.gl_pathc; j++) {
					args[i + j] =
					    strdup(glob_result.gl_pathv[j]);
				}
				args[i + glob_result.gl_pathc] = NULL;
			} else {
				// Si no hay coincidencias, deja el patrón original
				args[i] = strdup(args[i]);
			}
			globfree(&glob_result);
		}
	}
	return 1;
}

void
matar_procesos_background()
{
	// Ver si la variable de entorno KILLBACK está declarada
	if (getenv("KILLBACK") != NULL) {
		// Recorrer todos los procesos en segundo plano
		for (int i = 0; i < bg_pid_count; i++) {
			if (background_pids[i] > 0) {
				// Intentar matar el proceso con SIGKILL
				if (kill(background_pids[i], SIGKILL) != 0
				    && errno != ESRCH) {
					perror("Error al terminar proceso");
				}
				// Esperar a que el proceso termine para evitar zombies
				int status;

				waitpid(background_pids[i], &status, 0);
			}
		}
		// Reiniciar el contador de procesos en segundo plano
		bg_pid_count = 0;
	}
}

// Escribe el comando en el historial si es válido
void
escribir_historial(char *linea)
{
	if (last_result == 0) {
		// Escribe el comando en el archivo de historial
		fprintf(hist_file, "%s", linea);
		fflush(hist_file);	// Asegurarse de que los cambios se escriban inmediatamente
	}
}

// Ejecuta builtin
void
ejecutar_builtin(char **args)
{
	// cd
	if (strcmp(args[0], "cd") == 0) {
		// Si no hay argumentos intenta ir al HOME
		if (args[1] == NULL) {
			char *home = getenv("HOME");

			// Si no hay HOME, error
			if (home == NULL) {
				last_result = 1;
				setenv("result", "1", 1);
				fprintf(stderr,
					"No se pudo determinar el directorio HOME.\n");
				return;
			}
			// SI hay HOME va al HOME
			if (chdir(home) != 0) {
				// Si falla el cambio de directorio, se muestra un error
				last_result = 1;
				setenv("result", "1", 1);
				perror("cd");
			}
			// Si hay argumentos intenta ir a ese directorio
		} else if (chdir(args[1]) != 0) {
			// Si falla el cambio de directorio, se muestra un error
			last_result = 1;
			setenv("result", "1", 1);
			perror("cd");
		}
		last_result = 0;
		setenv("result", "0", 1);
		// exit
	} else if (strcmp(args[0], "exit") == 0) {
		fclose(hist_file);
		exit(0);
	} else if (strcmp(args[0], "pidsbg") == 0) {
		// Si tiene mas de un argumento
		if (args[1] != NULL) {
			fprintf(stderr, "usage: pidsbg\n");
			last_result = 1;
			setenv("result", "1", 1);
			return;
		}
		// Recorre el array de pids en segudno plano y los va imprimiendo
		for (int i = 0; i < bg_pid_count; i++) {
			if (background_pids[i] > 0) {
				fprintf(stdout, "%d\n", background_pids[i]);
			}
		}
		last_result = 0;
		setenv("result", "0", 1);
	} else if (strcmp(args[0], "nforks") == 0) {
		// Sin arguemntos imprime el numero de forks hasta el momento
		if (args[1] == NULL) {
			fprintf(stdout, "%d\n", fork_counter);
			// Si el argumento es -r, resetea el contador
		} else if (strcmp(args[1], "-r") == 0) {
			fork_counter = 0;
			// Si no, error
		} else {
			fprintf(stderr, "usage: nforks [-r]\n");
			last_result = 1;
			setenv("result", "1", 1);
			return;
		}

		last_result = 0;
		setenv("result", "0", 1);
	}
}

// Devuelve ruta si el archivo es ejecutable
char *
buscar_en_path(const char *cmd)
{
	// Obtiene el valor de la variable de entorno PATH
	char *path_env = getenv("PATH");

	if (!path_env)
		return NULL;

	// Se guarda una copia y divide por :
	char *path_copy = strdup(path_env);
	char *saveptr;
	char *dir = strtok_r(path_copy, ":", &saveptr);

	// Recorre todos los directorios del PATH
	while (dir) {
		// Se crea la ruta del comando de forma [directorio/comando]
		char full_path[PATH_MAX];

		snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);

		// Se comprueba si se peude ejecutar
		if (access(full_path, X_OK) == 0) {
			free(path_copy);
			return strdup(full_path);
		}
		// Si no se encuentra, sigue buscando en el siguiente directorio en PATH
		dir = strtok_r(NULL, ":", &saveptr);
	}

	free(path_copy);
	return NULL;
}

// Ejecuta el comando externo (no builtin)
void
ejecutar_externo(char **args, int fd_in, int fd_out, char *in_file,
		 char *out_file, int bg)
{
	pid_t pid = fork();

	// Contador de forks
	fork_counter++;

	if (pid < 0) {
		perror("fork");
		return;
	} else if (pid == 0) {
		if (fd_in != -1) {
			// Si se ha especificado un archivo de entrada (fd_in no es -1), redirigimos la entrada estándar
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
			// Si se ha especificado un archivo de salida (fd_out no es -1), redirigimos la salida estándar
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
			int status;

			// Espera
			waitpid(pid, &status, 0);
			if (WIFEXITED(status)) {
				last_result = WEXITSTATUS(status);
			} else {
				last_result = 1;
			}
		} else {
			background_pids[bg_pid_count++] = pid;
			printf("[ejecutando en segundo plano] PID: %d\n", pid);
		}
		char buf[16];

		snprintf(buf, sizeof(buf), "%d", last_result);
		setenv("result", buf, 1);
	}
}

// Tokeniza línea y ejecuta comando
void
ejecutar_linea(char *linea)
{
	char *args[MAX_TOKENS];
	int i = 0;
	char *saveptr;
	char *token = strtok_r(linea, " \t\r\n", &saveptr);
	char *in_file = NULL;
	char *out_file = NULL;

	int fd_in = -1;
	int fd_out = -1;
	int bg = 0;

	int here_document = 0;
	char here_buffer[MAX_CMD_LEN * 10] = { 0 };

	while (token && i < MAX_TOKENS - 1) {
		// Si hay redireccion de entrada y hay siguiente token se intenta abrir como lectura, si hay de salida se abre para escritura
		if (strcmp(token, "<") == 0
		    && (token = strtok_r(NULL, " \t\r\n", &saveptr))) {
			fd_in = open(token, O_RDONLY);
			// Error al abrir
			if (fd_in < 0) {
				perror("open");
				return;
			}
			in_file = token;
		} else if (strcmp(token, ">") == 0
			   && (token = strtok_r(NULL, " \t\r\n", &saveptr))) {
			fd_out =
			    open(token, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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

	// Si la linea de comando termina en HERE{ y si no hay ni redirecciones ni bg
	if (i > 0 && strcmp(args[i - 1], "HERE{") == 0) {
		if (fd_in != -1 || fd_out != -1 || bg) {
			fprintf(stderr,
				"HERE{ no puede combinarse con redirección de entrada o ejecución en segundo plano.\n");
			return;
		}
		// Borra el HERE{
		args[i - 1] = NULL;
		here_document = 1;
		printf("--> ");
		fflush(stdout);
		char temp_line[MAX_CMD_LEN];

		// Lee todas las lineas y las va almacenando en un buffer temporal hasta que se detecta el }
		while (fgets(temp_line, sizeof(temp_line), stdin)) {
			if (strcmp(temp_line, "}\n") == 0)
				break;
			strncat(here_buffer, temp_line,
				sizeof(here_buffer) - strlen(here_buffer) - 1);
			printf("--> ");
			fflush(stdout);
		}
	}

	args[i] = NULL;

	// Expansión de globbing
	expandir_globbing(args);

	// Linea vacia
	if (args[0] == NULL)
		return;

	// Solo continua la ejecucion si se consigue asignar la variable de entorno
	if (!reemplazar_variables(args)) {
		return;
	}

	matar_procesos_background();

	if (es_builtin(args[0])) {
		ejecutar_builtin(args);
	} else {
		if (here_document) {
			// Crea un pipe para comunicar el contenido del here (0 --> leer) (1 --> escritura)
			int pipefd[2];

			if (pipe(pipefd) == -1) {
				perror("pipe");
				return;
			}
			// Escribe en el extremo de escritura del pipe el contenido del here
			write(pipefd[1], here_buffer, strlen(here_buffer));
			close(pipefd[1]);
			// Ejecuta el comando externo pasando como entrada estándar (fd_in)
			ejecutar_externo(args, pipefd[0], fd_out, NULL,
					 out_file, bg);
			close(pipefd[0]);
		} else {
			ejecutar_externo(args, fd_in, fd_out, in_file, out_file,
					 bg);
		}
	}
}

int
main()
{
	char linea[MAX_CMD_LEN];

	// Sacar el PATH a HOME
	char *home = getenv("HOME");

	if (!home) {
		fprintf(stderr,
			"Error: No se pudo obtener el directorio HOME.\n");
		return 1;
	}
	// Se crea o trunca ./hist_shell
	char historial_path[PATH_MAX];

	snprintf(historial_path, sizeof(historial_path), "%s/.hist_myshell",
		 home);
	hist_fd = open(historial_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	// Se abre como escritura
	if (hist_fd != -1) {
		hist_file = fdopen(hist_fd, "w");
		if (!hist_file) {
			perror("fdopen");
			return 1;
		}
	}

	setenv("result", "0", 1);

	while (1) {
		printf("> ");
		fflush(stdout);

		if (!fgets(linea, sizeof(linea), stdin)) {
			// EOF o error
			break;
		}
		escribir_historial(linea);
		ejecutar_linea(linea);
	}

	printf("\n");
	fclose(hist_file);
	return 0;
}
