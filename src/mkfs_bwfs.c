/**
 *  En este codigo como actualizacion
 * 
 *  Muestra la creacion de un nuevo FS BWFS, donde se inicializa el bwfs_t, crea
 *  bwfs_metadata.bin y backing files.
 * 
 *  Luego se establece el máximo bloque de 1000x1000px en la constante de
 *  MAX_BLOCK_BYTES y validación en main 
 * 
 *  Se utilizan los inodos como indexación, estructura de inode_t incluida bwfs_t
 * 
 *  FS distribuido, Crea 1024 archivos (file_0000.dat … file_1023.dat) en el storage.
 *  
 *  Configuración desde ini, Función read_config lee storage_path y max_block_bytes.
 * 
 *  Sintaxis mkfs.bwfs -c config.ini, Validación de argumentos al inicio
 * 
 *  Explicacion sobre diseño:
 *  Creacion de 1024 archivos vacíos dentro del directorio de almacenamiento, con el objetivo
 *  de que represente un bloque de datos de forma independiente, entonces en lugar de tener un único
 *  archivo grande, el FS se distribuye en muchos archivos pequeños. Esto facilita la gestión, donde
 *  se puede mapear cada archivo a un inodo y simular un sistema que sea distribuido
 * 
 *  Para la especificacion de que cada bloque BWFS debe tener un máximo de 1000x10000px, en
 *  el bloque para el archivo file_xxx.dat puede que almacene 1 000 000 bytes de forma equivalente
 *  a una matriz de 1000x1000 píxeles considerando que cada píxel necesita 1 byte. La forma que se puede
 *  interpretar es que cada archivo es un contenedor de imagen, entonces el limite asegura que ningun bloque
 *  supere el tamaño máximo definido por la especificación.
 * 
 *  El funcionamiento de los inodos de indexación, donde cada inodo corresponde a un archivo
 *  dentro del FS, used es para indicar que si el inodo esta ocupado, name es el nombre del archivo logico dentro
 *  del FS, size es el tamaño del archivo, mode para los permisos y tipo. Como funcion los inodos son la tabla 
 *  de indexacion que enlaza los nombres de archivos del FS con los backing files (file_xxxx.dat). Cuando el 
 *  usuario ingresa el comando touch para crear un archivo pues se asigna un inodo libre y se enlaza con un bloque 
 *  fisico
 * 
 *  Para lograr que sea distribuido, El FS no guarda todo en un único archivo, en cambio se tiene que
 *  Metadata (bwfs_metadata.bin) guarda fingerprint, parámetros y tabla de inodos, Storage (bwfs_storage/)
 *  contiene 1024 backing files. Cada archivo lógico del FS puede mapearse a uno o varios backing files. Esto simula
 *  un sistema distribuido los datos están repartidos en múltiples contenedores.
 * 
 *  Para obtener la configuración de .ini con la función read_config, abre config.ini y busca parámetros
 *  storage_path donde crea la ruta que se crean los backing files, max_block_byte es el tamño máximo de bloque
 *  . Si no se encuentra los valores válido pues usa defaults (bwfs_storage, MAX_BLOCK_BYTES).
 * 
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

// Archivo del metadata donde se almacena el fingerprint y parámetros
#define META_FILE "bwfs_metadata.bin"
// Número máximo de archivos de datos
#define MAX_FILES 1024
// Máximo tamaño de bloque
#define MAX_BLOCK_BYTES 1000000
// Fingerprint BWFS = "BWFS"
#define BWFS_MAGIC 0x42574653u

// --------- ESTRUCTURAS ---------

typedef struct {
    int used;          // Flag: 0 libre, 1 usado
    char name[256];    // Nombre del archivo
    size_t size;       // Tamaño
    int mode;          // Permisos / tipo
} inode_t;

typedef struct {
    unsigned int magic;              // Identificador
    char storage_path[512];          // Ruta absoluta del storage
    size_t max_block_bytes;          // Tamaño máximo por archivo
    inode_t inodes[MAX_FILES];       // Tabla de inodos
} bwfs_t;


// --------- LECTURA DE CONFIG ---------

static int read_config(const char *filename, char *storage_path, size_t *max_block_bytes) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("config.ini");
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "storage_path = %511s", storage_path) == 1) continue;
        if (sscanf(line, "max_block_bytes = %zu", max_block_bytes) == 1) continue;
    }

    fclose(f);
    return 0;
}


// --------- NORMALIZAR RUTA ---------

static void make_absolute_path(char *dst, size_t dstsz, const char *storage_path) {
    if (storage_path[0] == '/') {
        strncpy(dst, storage_path, dstsz - 1);
        dst[dstsz - 1] = '\0';
        return;
    }

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        strncpy(dst, storage_path, dstsz - 1);
        dst[dstsz - 1] = '\0';
        return;
    }

    snprintf(dst, dstsz, "%s/%s", cwd, storage_path);

    // Quitar dobles barras
    for (char *p = dst; *p; ++p) {
        if (p[0] == '/' && p[1] == '/') {
            memmove(p, p + 1, strlen(p));
        }
    }
}


// --------- PROGRAMA PRINCIPAL ---------

int main(int argc, char **argv) {

    if (argc != 3 || strcmp(argv[1], "-c") != 0) {
        printf("Uso: mkfs.bwfs -c config.ini\n");
        return 1;
    }

    // Valores por defecto
    char storage_path_cfg[512] = "bwfs_storage";
    size_t max_block_bytes_cfg = MAX_BLOCK_BYTES;

    // Leer config.ini
    if (read_config(argv[2], storage_path_cfg, &max_block_bytes_cfg) != 0) {
        fprintf(stderr, "Advertencia: Error leyendo config.ini, usando valores por defecto.\n");
    }

    // Validar límite
    if (max_block_bytes_cfg > MAX_BLOCK_BYTES) {
        fprintf(stderr, "max_block_bytes (%zu) excede límite, ajustando.\n", max_block_bytes_cfg);
        max_block_bytes_cfg = MAX_BLOCK_BYTES;
    }

    // Crear metadata
    bwfs_t meta;
    memset(&meta, 0, sizeof(meta));

    meta.magic = BWFS_MAGIC;
    meta.max_block_bytes = max_block_bytes_cfg;

    make_absolute_path(meta.storage_path, sizeof(meta.storage_path), storage_path_cfg);

    // Inicializar explícitamente cada inodo
    for (int i = 0; i < MAX_FILES; ++i) {
        meta.inodes[i].used = 0;
        meta.inodes[i].name[0] = '\0';
        meta.inodes[i].size = 0;
        meta.inodes[i].mode = 0;
    }

    // Crear directorio de storage
    if (mkdir(meta.storage_path, 0755) != 0 && errno != EEXIST) {
        perror("mkfs: mkdir storage_path");
        return 1;
    }

    // ELIMINAR metadata previa
    unlink(META_FILE);

    // Guardar metadata limpia
    FILE *fm = fopen(META_FILE, "wb");
    if (!fm) {
        perror("mkfs: metadata fopen");
        return 1;
    }

    if (fwrite(&meta, sizeof(meta), 1, fm) != 1) {
        perror("mkfs: metadata fwrite");
        fclose(fm);
        return 1;
    }
    fclose(fm);

    // Crear archivos backend
    char path[PATH_MAX];
    for (int i = 0; i < MAX_FILES; ++i) {
        snprintf(path, sizeof(path), "%s/file_%04d.dat", meta.storage_path, i);
        FILE *f = fopen(path, "wb");
        if (!f) {
            perror("mkfs: fopen backing file");
            return 1;
        }
        fclose(f);
    }

    printf("mkfs.bwfs: FS creado en '%s' con %d archivos de datos.\n",
           meta.storage_path, MAX_FILES);

    return 0;
}
