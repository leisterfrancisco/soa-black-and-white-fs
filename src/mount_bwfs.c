/**
 * mount.bwfs - Monta el sistema de archivos BWFS.
 * 
 * Funcionamiento:
 *   1. Debe ejecutarse así:
 *          mount.bwfs -c config.ini mnt/
 *   2. En caso de no haber metadata pues falla (no se ejecutó mkfs.bwfs antes), por lo
 *   que asegura que el usuario debe iniciar el FS previo a montarlo
 *   3. Ademas crea el directorio donde estará el montaje solo si el metadata existe
 *   
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#include <errno.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define META_FILE "bwfs_metadata.bin"


/**
 * Covierte la ruta relativa en absoluta
 * Entonces si el path ya es absoluto, solo lo copia. En caso de ser relativo, lo concatena con el
 * getcwd
 * 
 */
static void make_absolute(char *out, size_t outsz, const char *path) {
    // Si inicia con '/' pues es absoluta, lo copia
    if (path[0] == '/') {
        strncpy(out, path, outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }
    // Busca convertir el relativo 
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));
    snprintf(out, outsz, "%s/%s", cwd, path);
}


/**
 * Realiza una verificacion si existe la metadata (bwfs_metadata.bin), por lo tanto
 * en caso que exista devuelve un 1 y si no existe un 0 
 */
static int metadata_exists(void) {
    return access(META_FILE, F_OK) == 0;
}

/**
 * Programa principal
 */
int main(int argc, char *argv[]) {

    // Se realiza una validacion de sintaxis 
    //  mount.bwfs -c config.ini mnt/
    if (argc < 3) {
        fprintf(stderr, "Uso: %s -c config.ini mnt/\n", argv[0]);
        return 1;
    }

    // path hacia config.ini
    const char *config = NULL;
    // punto de montaje
    const char *mnt = NULL;

    /**
     * Busca "-c config.ini", el ultimo parametro es el punto de montaje
     * 
     */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config = argv[++i];
        } else {
            mnt = argv[i];
        }
    }
    // Validacion que ambos existen
    if (!config || !mnt) {
        fprintf(stderr, "Uso: %s -c config.ini mnt/\n", argv[0]);
        return 1;
    }

    /**
     * Se convierte el config.ini en ruta absoluta, realpath falla si el archivo no existe
     * 
     */
    char abs_config[PATH_MAX];
    if (!realpath(config, abs_config)) {
        perror("mount.bwfs: realpath(config.ini)");
        return 1;
    }

    /**
     * Determina el directorio donde vive config.ini, en dirname el cual modifica el 
     * string, lo copiamos primero
     * 
     */
    char cfg_copy[PATH_MAX];
    strncpy(cfg_copy, abs_config, sizeof(cfg_copy));
    char *config_dir = dirname(cfg_copy);

    /**
     * Cambia cwd al directorio del proyecto, esto es critico porque
     *   - bwfs_metadata.bin está ahí
     *   - bwfs_storage está ahí
     *   - el ejecutable "bwfs" está ahí
     */
    if (chdir(config_dir) != 0) {
        perror("mount.bwfs: chdir");
        return 1;
    }

    // verificar si metadata existe antes de montar, en caso que no, no se monta
    // ni se crea el directorio
    if (!metadata_exists()) {
        fprintf(stderr,
                "ERROR: No existe metadata válida.\n"
                "Debe ejecutar primero: mkfs.bwfs -c config.ini\n");
        return 1;
    }

    // Resuelve el mnt como una ruta absoluta, entonces si el realpath falla, que es tipico
    // cuando el directorio esta vacio, pues entonces intentamos crearlo solo si metadata existe
    char abs_mount[PATH_MAX];
    if (!realpath(mnt, abs_mount)) {
        // Intentar crear directorio
        if (mkdir(mnt, 0777) != 0 && errno != EEXIST) {
            perror("mount.bwfs: no se pudo crear directorio de montaje");
            return 1;
        }
        // Convierte al path absoluto de forma manual
        make_absolute(abs_mount, sizeof(abs_mount), mnt);
    }

    // Obtiene la ruta absoluta del ejecutable real, ./bwfs
    char exe_path[PATH_MAX];
    if (!realpath("./bwfs", exe_path)) {
        perror("mount.bwfs: no se encontró bwfs");
        return 1;
    }
    // Mensaje para debugging
    printf("Ejecutando: %s -c '%s' '%s' -f\n",
           exe_path, abs_config, abs_mount);
    
    // Ejecuta el FS real, reemplaza este proceso
    execl(exe_path, exe_path, "-c", abs_config, abs_mount, "-f", NULL);

    perror("mount.bwfs: execl falló"); // En caso que llega aca, hubo fallo
    return 1;
}
