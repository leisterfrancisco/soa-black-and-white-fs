 /**
  * En este programa se implementa el comando de mount.bwfs, se encarga de montar 
  * el sistema de archivos BWFS siguiendo una clara sintaxis requerida
  * 
  *         `mount.bwfs -c config.ini mnt/`
  * 
  * Tiene como objetivo en validar los parámetros, convertir tanto el archivo de 
  * configuración como el punto de montaje a rutas absolutas, de forma que se posiciona
  * en el directorio donde se encuentre el FS y esto permite que el bwfs encuentre
  * el bwfs_metadata.bin y bwfs_storage (era un error en versiones anteriores) y al final
  * en realiza la ejecución del FS real con los argumentos adecuados. Con esto, se cumple
  * con el requisito de permitir al usuario en especificar el archivo que define el incio
  * del FS y montar el BWFS de forma correcta en un directorio del sistema operativo.
  * 
  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <libgen.h>

/**
 * En el make absolute
 * Se construye una ruta absoluta de forma manual, y existe para que cuando el realpath
 * falla si el directorio esta vacío, por lo que esta función funciona como un fallback seguro
 * 
 * Parte de los parámetros, el out se refiere donde quedaría la ruta absoluta, luego el outsz es
 * el tamaño del buffer y el path pues la ruta original
 */


static void make_absolute(char *out, size_t outsz, const char *path) {
    if (path[0] == '/') {
        // La ruta es absoluta, por lo que simplemente copia
        strncpy(out, path, outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));
    // Formato CWD, / y el relative path
    snprintf(out, outsz, "%s/%s", cwd, path);
}

// Flujo completo del mount.bwfs
int main(int argc, char *argv[]) {

    /**
     * Se realiza la validacion de la sintaxis exacta del enunciado que se solicita
     * 
     *              mount.bwfs -c config.ini mnt/
     * 
     * 
     */
    if (argc < 3) {
        fprintf(stderr, "Uso: %s -c config.ini mnt/\n", argv[0]);
        return 1;
    }

    // Archivo de configuración
    const char *config = NULL;
    // Punto del montaje
    const char *mnt = NULL;

    /**
     * Ahora se parsean los argumentos, entonces se deberían de encontrar
     * 
     *          -c config.ini
     * además del 
     *          mnt/
     * 
     * 
     */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            // siguiente argumento es para el config.ini
            config = argv[++i];
        } else {
            // para el resto del mnt
            mnt = argv[i];
        }
    }

    if (!config || !mnt) {
        fprintf(stderr, "Uso: %s -c config.ini mnt/\n", argv[0]);
        return 1;
    }
    /**
     * Ahora se convierte el config.ini a una ruta absoluta que sea real
     *      abs_config = realpath(config.ini)
     * 
     * Si el realpath falla, no se puede continuar, entonces el config.ini no existe
     * 
     * 
     */
    char abs_config[PATH_MAX];
    if (!realpath(config, abs_config)) {
        perror("mount.bwfs: realpath(config.ini)");
        return 1;
    }

    /**
     * Continuando se determina el directorio donde se encuentra el config.ini
     * 
     * El dirname modifica la cadena, entonces por eso se realiza una copia
     * 
     * El directorio es necesario porque se encuentran los archivos
     *          bwfs_metadata.bin
     *          bwfs_storage/
     *          bwfs   (el ejecutable real)
     * 
     * FUSE trabaja en base al directorio actual del proceso, entonces si se cambia
     * el cwd para que todo sea encontrado de forma correcta
     * 
     */
    char cfg_copy[PATH_MAX];
    strncpy(cfg_copy, abs_config, sizeof(cfg_copy));

    char *config_dir = dirname(cfg_copy);

    /**
     * En el siguiente bloque se cambia el cwd al directorio del proyecto como tal
     * 
     * Entonces lo que realiza es, el ejecutable bwfs se encuentre en el ./bwfs, el
     * metada bwfs_metadata.bin se encuentre de forma correcta y que el
     * bwfs_storage funciones sin problemas
     * 
     */
    if (chdir(config_dir) != 0) {
        perror("mount.bwfs: chdir");
        return 1;
    }

    /**
     * Se resuelve la ruta absoluta del punto de montaje.
     * Entonces el realpath(mnt) falla en caso que el directorio se encuentre vacío
     * 
     * Por eso si el realpath falla, se realiza la reconstruccion de la ruta absoulta 
     * de forma manual utilizando make_absolute()
     * 
     */
    char abs_mount[PATH_MAX];

    if (!realpath(mnt, abs_mount)) {
        // Esto ocurre en WSL cuando el directorio está vacío
        make_absolute(abs_mount, sizeof(abs_mount), mnt);
    }

    /**
     * Ahora se localiza el ejecutable real del bwfs, donde debe de estar
     * en el mismo directorio que el config.ini entonces en el caso que no 
     * se encuentre pues no se puede montar el FS
     * 
     */
    char exe_path[PATH_MAX];
    if (!realpath("./bwfs", exe_path)) {
        perror("mount.bwfs: no se encontró bwfs");
        return 1;
    }

    /**
     * Se muestra en pantalla el comando final que se ejecutará, entonces esto 
     * ayuda mucho para realiza debugging
     */
    printf("Ejecutando: %s -c '%s' '%s' -f\n", exe_path, abs_config, abs_mount);

    /**
     * Uno de los bloques mas fundamentales, se ejecuta el FS real, reemplaza 
     * completamente este proceso. Entonces configura el storage path y el max block bytes
     * el punto de montaje, el modo FUSE y en caso que el execl funcione pues este proceso
     * desaparece.
     */


    execl(exe_path, exe_path, "-c", abs_config, abs_mount, "-f", NULL);

    /**
     * En caso de llegar en este bloque pues el execl falla y no reemplaza el proceso
     */
    
    perror("mount.bwfs: execl falló");
    return 1;
}