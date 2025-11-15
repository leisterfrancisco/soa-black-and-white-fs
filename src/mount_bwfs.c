/**
 * En esta version de mount_bwfs 
 * 
 * Valida la sintaxis: mount.bwfs -c config.ini mnt/
 * Convierte rutas a absolutas para config.ini y mnt/, daba errores en la version anterior
 * Fija cwd al directorio de config.ini para asegurar que bwfs se ejecute en el contexto correcto
 * Localiza el ejecutable bwfs en el mismo directorio que config.ini
 * Ejecuta bwfs con los argumentos correctos
 * 
 * En resumen, este codigo es un wrapper de montaje que prepara el entonrno y lanza bwfs
 * de forma segura.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <libgen.h>

int main(int argc, char *argv[]) {
    // Validación de argumentos
    // Entonces recibe: mount.bwfs -c config.ini mnt/
    if (argc < 3) {
        fprintf(stderr, "Uso: %s -c config.ini mnt/\n", argv[0]);
        return 1;
    }

    // Ruta del archivo de configuración (config.ini) 
    const char *config = NULL;
    // Punto de montaje (mnt/)
    const char *mnt = NULL;

    // Recorrido de argv para identificación de "-c config.ini" y mountpoint (mnt)
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config = argv[++i]; // Guardar ruta del archivo de configuración
        } else {
            mnt = argv[i]; // Guarda la ruta del punto de montaje
        }
    }

    // Validar que ambos parámetros existan
    if (!config || !mnt) {
        fprintf(stderr, "Uso: %s -c config.ini mnt/\n", argv[0]);
        return 1;
    }

    // Realiza la conversión de config.init a una ruta absoluta
    // Lo que hace es evitar problemas del cwd y asegura la reproducibilidad
    char abs_config[PATH_MAX];
    if (!realpath(config, abs_config)) {
        // En el caso que realptah falle, copia la ruta tal cual
        strncpy(abs_config, config, sizeof(abs_config)-1);
        abs_config[sizeof(abs_config)-1] = '\0';
    }

    // Ahora realiza la conversión de mountpoint a una ruta absoluta
    char abs_mount[PATH_MAX];
    if (!realpath(mnt, abs_mount)) {
        // Si realpath falla, se copia la ruta tal cual
        strncpy(abs_mount, mnt, sizeof(abs_mount)-1);
        abs_mount[sizeof(abs_mount)-1] = '\0';
    }

    // Ahora usa el directorio de config.ini como cwd del proyecto
    // entonces el dirname() devuelve el directorio padre de la ruta absoluta
    // de configi,ini   
    char cfg_copy[PATH_MAX];
    strncpy(cfg_copy, abs_config, sizeof(cfg_copy)-1);
    cfg_copy[sizeof(cfg_copy)-1] = '\0';
    char *dir = dirname(cfg_copy);   // dirname hace la modificacion de la cadena

    // Realiza el cambio de cwd al directorio del proyecto (donde están bwfs_metadata.bin y bwfs_storage)
    if (chdir(dir) != 0) {
        perror("mount.bwfs: chdir falló");
        return 1;
    }

    // Se obtiene la ruta absoluta al ejectable bwfs
    // Entonces asume que bwfs esta en el mismo directorio que config.ini
    char exe_path[PATH_MAX];
    if (!realpath("./bwfs", exe_path)) {
        perror("mount.bwfs: no se encontró bwfs");
        return 1;
    }

    // Imprime el resultado al usuario el comando que se ha a ejecutar, es mas
    // informativo para debugging
    printf("Ejecutando: %s -c '%s' '%s' -f\n", exe_path, abs_config, abs_mount);

    // Realiza el reemplazo del proceso actual por bwfs, entonces pasa los argumentos
    // correctos:
    // -c abs_config : archivo de configuración
    // abs_mount     :punto de montaje
    // -f            : foreground (no se va a background)
    execl(exe_path, exe_path, "-c", abs_config, abs_mount, "-f", NULL);

    // En el caso que execl falle, se informa el error
    perror("mount.bwfs: execl falló");
    return 1;
}
