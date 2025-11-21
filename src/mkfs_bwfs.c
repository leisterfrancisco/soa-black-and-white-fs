/**
 * Programa que se busca crear un file system BWFS
 * 
 * Como funcionalidades:
 * - Inicializa el metadata del FS en el bwfs_t
 * - Luego se crear el archivo denominado como bwfs_metadata.bin que contiene la tabla de 
 * inodos y parámetros
 * - Se crea 1024 archivos físicos en el storage
 * - Permite la configuración por medio del archivo config.init
 * 
 * Parte del diseño:
 *  - Tiene que cada archivo físico representa un bloque independiente del FS
 *  - Además cada inodo corresponde a un archivo lógico dentro del FS
 *  - El archivo del metada (bwfs_metadata.bin) almacena fingerprint, parámetros y tabla de inodos
 *  - Ademas el storage, es decir el directorio con los backing files contiene los bloque físicos
 *  - Simula un FS distribuido, donde los datos están repartidos en múltiples contenededores
 * 
 * Comando para ejecutar:
 *  ./mkfs.bwfs -c config.ini
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Archivo del metadata donde se almacena el fingerprint y parámetros del FS
#define META_FILE "bwfs_metadata.bin"
// Número máximo de archivos de archivos (inodos) en el FS
#define MAX_FILES 1024
// Máximo tamaño de bloque en bytes
#define MAX_BLOCK_BYTES 1000000
// Fingerprint BWFS = "BWFS" en hexadecimal
#define BWFS_MAGIC 0x42574653u
 
// --------- ESTRUCTURAS ---------

// Inodo, que representa un archivo lógico dentro del FS
typedef struct {
    int used;          // Flag: 0 libre, 1 usado
    char name[256];    // Nombre del archivo
    size_t size;       // Tamaño
    int mode;          // Permisos / tipo
} inode_t;

// Metadata global del FS
typedef struct {
    unsigned int magic;              // Identificador
    char storage_path[512];          // Ruta absoluta del storage
    size_t max_block_bytes;          // Tamaño máximo por archivo
    inode_t inodes[MAX_FILES];       // Tabla de inodos
} bwfs_t;


// --------- LECTURA DE CONFIG ---------

// Lectura de la configuración desde el config.ini
// Los parámetros buscados, storage_path y max_block_bytes
static int read_config(const char *filename, char *storage_path, size_t *max_block_bytes) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("config.ini");
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Busca y lee el storage path
        if (sscanf(line, "storage_path = %511s", storage_path) == 1) continue;
        // Busca y lee el max block bytes
        if (sscanf(line, "max_block_bytes = %zu", max_block_bytes) == 1) continue;
    }

    fclose(f);
    return 0;
}


// --------- NORMALIZAR RUTA ---------
// Convierte una ruta relativa en absoluta y elimina las barras dobles
static void make_absolute_path(char *dst, size_t dstsz, const char *storage_path) {
    // Si es una ruta absoluta, copia de forma directa
    if (storage_path[0] == '/') {
        strncpy(dst, storage_path, dstsz - 1);
        dst[dstsz - 1] = '\0';
        return;
    }
    // Se obtiene el directorio actual
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        strncpy(dst, storage_path, dstsz - 1);
        dst[dstsz - 1] = '\0';
        return;
    }
    // Contruccion de ruta relativa
    snprintf(dst, dstsz, "%s/%s", cwd, storage_path);

    // Quitar dobles barras
    for (char *p = dst; *p; ++p) {
        if (p[0] == '/' && p[1] == '/') {
            memmove(p, p + 1, strlen(p));
        }
    }
}


// Programa principal

int main(int argc, char **argv) {

    // Validacion de sintaxis, mkfs.bwfs -c config.ini
    if (argc != 3 || strcmp(argv[1], "-c") != 0) {
        fprintf(stderr, "Uso: mkfs.bwfs -c config.ini\n");
        return 1;
    }

    const char *config_file = argv[2];

    // Comprobar que config.ini existe
    char abs_config[PATH_MAX];
    if (!realpath(config_file, abs_config)) {
        fprintf(stderr, "mkfs.bwfs: No se encontró el archivo de configuración '%s'\n",
                config_file);
        return 1;
    }

    // Lectura de parametros desde el config.ini
    char storage_path_cfg[512] = "bwfs_storage";
    size_t max_block_bytes_cfg = MAX_BLOCK_BYTES;

    if (read_config(abs_config, storage_path_cfg, &max_block_bytes_cfg) != 0) {
        fprintf(stderr,
                "mkfs.bwfs: Advertencia: error leyendo config.ini, usando valores por defecto.\n");
    }
    // Ajuste de max block byte si excede el limite permitido
    if (max_block_bytes_cfg > MAX_BLOCK_BYTES) {
        fprintf(stderr,
                "mkfs.bwfs: max_block_bytes (%zu) excede límite permitido, ajustando.\n",
                max_block_bytes_cfg);
        max_block_bytes_cfg = MAX_BLOCK_BYTES;
    }

    // Evitar sobreescribir un FS existente
    if (access(META_FILE, F_OK) == 0) {
        fprintf(stderr,
                "mkfs.bwfs: ERROR: el FS ya existe. Primero elimine '%s' si desea reformatear.\n",
                META_FILE);
        return 1;
    }

    // Ahora se inicializa metada limpia en RAM
    bwfs_t meta;
    memset(&meta, 0, sizeof(meta));

    meta.magic = BWFS_MAGIC;                     // fingerprint requerido
    meta.max_block_bytes = max_block_bytes_cfg;  // max block válido

    make_absolute_path(meta.storage_path,
                       sizeof(meta.storage_path),
                       storage_path_cfg);

    // Inicializa la tabla de nodos vacía
    for (int i = 0; i < MAX_FILES; ++i) {
        meta.inodes[i].used = 0;
        meta.inodes[i].name[0] = '\0';
        meta.inodes[i].size = 0;
        meta.inodes[i].mode = 0;
    }

    // Creacion del directorio storage
    if (mkdir(meta.storage_path, 0755) != 0 && errno != EEXIST) {
        perror("mkfs.bwfs: mkdir(storage)");
        return 1;
    }

    // Creacion de archivo bwfs_metadata.bin desde cero
    FILE *fm = fopen(META_FILE, "wb");
    if (!fm) {
        perror("mkfs.bwfs: fopen(metadata)");
        return 1;
    }

    if (fwrite(&meta, sizeof(meta), 1, fm) != 1) {
        perror("mkfs.bwfs: fwrite(metadata)");
        fclose(fm);
        return 1;
    }
    fclose(fm);

    // Creacion de archivos backend del FS, entonces se crean los siguientes
    // file_XXXX.dat
    char path[PATH_MAX];

    for (int i = 0; i < MAX_FILES; ++i) {
        snprintf(path, sizeof(path), "%s/file_%04d.dat", meta.storage_path, i);
        FILE *f = fopen(path, "wb");
        if (!f) {
            perror("mkfs.bwfs: creando archivo backend");
            return 1;
        }
        fclose(f);
    }

    // Mensajes de debbuging
    printf("mkfs.bwfs: FS creado correctamente.\n");
    printf("  Storage path: %s\n", meta.storage_path);
    printf("  Máx bloque:   %zu bytes\n", meta.max_block_bytes);
    printf("  Archivos creados: %d\n", MAX_FILES);

    return 0;
}

