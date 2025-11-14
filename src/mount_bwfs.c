'''
Monta el BWFS en el punto de montaje especificado utilizando la configuración dada.
Sintaxis: mount.bwfs -c config.ini mnt
Llama al daemon con los parametros adecuados

Falta que el codigo solo permita especificar el archivo de configuracion config.init
No implementa logica de fingerprint automatico como ejemplo detectar metadata existente
'''

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <libgen.h>

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s -c config.ini mnt/\n", argv[0]);
        return 1;
    }

    const char *config = NULL;
    const char *mnt = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config = argv[++i];
        } else {
            mnt = argv[i];
        }
    }

    if (!config || !mnt) {
        fprintf(stderr, "Uso: %s -c config.ini mnt/\n", argv[0]);
        return 1;
    }

    char abs_mount[PATH_MAX];
    if (!realpath(mnt, abs_mount)) {
        strncpy(abs_mount, mnt, sizeof(abs_mount)-1);
        abs_mount[sizeof(abs_mount)-1] = '\0';
    }

    char exe_path[PATH_MAX];
    realpath(argv[0], exe_path);
    char *dir = dirname(exe_path);
    if (chdir(dir) != 0) {
        perror("mount.bwfs: chdir falló");
        return 1;
    }

    printf("Ejecutando: ./bwfs -c '%s' '%s' -f\n", config, abs_mount);

    // Llamar directamente a bwfs con los argumentos correctos
    execlp("./bwfs", "./bwfs", "-c", config, abs_mount, "-f", NULL);

    perror("mount.bwfs: execlp falló");
    return 1;
}
