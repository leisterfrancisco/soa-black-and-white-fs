/**
 * BWFS - Implementación con FUSE parte de inicialización, metadata, rutas
 * y gestión de inodos

*/

/**
 *  Primera Parte, carga la metada del FS (fingerprint, storage_path, max_block_bytes, tabla de inodos) y luego lo normaliza para que sea 
 *  segura y manejable. Mantiene una tabla de inodos en memoria con atributos POSIX para integrarse con las operaciones FUSE. Luego asegura
 *  que el directorio de almacenamiento exista y que las rutas no estén corruptas. Permite asignar un inodo nuevo y crear su respectivo
 *  archivo fisico distribuido en bwfs_storage, persistiendo el cambio en bwfs_metadata.bin. Prepara el FS para que las operaciones 
 *  FUSE puedan trabajar sobre esta estructura
 * 
 *  Segunda Parte, Se implementan las operaciones básicas de FUSE que permiten que BWFS se comporte como un sistema de archivos real, cuando el
 *  usuario ejecuta comandos en el punto de montaje, FUSE invoca estas funciones, entonces:
 *     1. bwfs_getattr devuelve atributos de archivos y del directorio raiz (lo que hace posible ls -l o stat)
 *     2. bwfs_readdir lista el contenido del directorio raíz mostrando los nombres de los inodos activos
 *     3. bwfs_create reserva un inodo libre y lo inicializa para crear un nuevo archivo (touch)
 *     4. bwfs_unlink libera un inodo y borra el archivo (rm)
 *    -extra bwfs_utimens(extra ayuda para solucionar un error en consola) actualiza los tiempos de acceso y modificación (touch actualiza atime/mtime)
 * 
 *  Luego estos cambios se guardan en bwfs_metadata.bin mediante save_metadata, asegurando pesistencia, finalmente la tabla bwfs_oper hace un registro
 *  de estas funciones y main arranca FUSE con ellas, de forma que cualquier operacion del usuario en el directorio montado se traduzca en llamadas a estas
 *  funciones
 */
 
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Parametros de limite y nombres
#define MAX_FILES 1024  // Número máximo de inodos y backing files
#define MAX_NAME_LEN 255 // Longitud máxima de nombre de archivo
#define META_FILENAME "/home/elias/DesktopWSL/soa-black-and-white-fs/bwfs_metadata.bin"      // Ruta importante modificar donde se encuentre el archivo bwfs_metadata.bin!!
#define BWFS_MAGIC 0x42574653u // "BWFS" fingerprint 

// Estructuras de DISCO(persistencia) y MEMORIA(runtime)
// Inodo en disco: lo que se guarda dentro de bwfs_metadata.bin
typedef struct {
    int used;   // 1 si el inodo está ocupado, 0 si libre
    char name[256]; // Nombre del archivo lógico
    size_t size; // Tamaño lógico del archivo (bytes)
    int mode; // Permisos/tipo (informativo en disco)
} inode_disk_t;

// Metadata completa que se guarda en disco
typedef struct {
    unsigned int magic;   // Fingerprint "BWFS", valida el archivo
    char storage_path[512];  // Ruta donde viven los backing files
    size_t max_block_bytes; // Límite de tamaño por bloque (1000x1000, 1 000 000 bytes)
    inode_disk_t inodes[MAX_FILES]; // Tabla de inodos persistente
} bwfs_disk_t;

// Inodo en memoria: versión enriquecida para FUSE (POSIX attrs)
typedef struct {
    int used;  // Ocupado/libre
    char name[MAX_NAME_LEN + 1];  // Nombre del archivo
    mode_t mode;  // Modo POSIX (S_IFREG | 0644, etc.)
    uid_t uid; // Propietario
    gid_t gid; // Grupo
    size_t size; // Tamaño lógico
    time_t atime; // Último acceso
    time_t mtime;  // Última modificación
    time_t ctime; // Último cambio de metadata
} inode_t;

// Estado del FS en memoria mientras FUSE está montado
typedef struct {
    inode_t inodes[MAX_FILES]; // Tabla de inodos en RAM
    char storage_path[512]; // Ruta normalizada del storage
    size_t max_block_bytes; // Tamaño máximo por bloque
} bwfs_t;

static bwfs_t bwfs; // Instancia global del FS en memoria

// Unidades de normalización de las rutas, evitar y las rutas malas

// Copia src en dst eliminando "dobles barras" consecutivas (// -> /)
static void compact_slashes(const char *src, char *dst, size_t dstsz) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dstsz; ++i) {
        dst[j++] = src[i];
        if (src[i] == '/') {
            while (src[i + 1] == '/') i++; // Salta barras duplicadas
        }
    }
    dst[j] = '\0'; // Termina cadena
}

// Normaliza bwfs.storage_path:
// - Si es absoluta, solo compacta barras.
// - Si es relativa, la convierte a absoluta usando el cwd.
static void normalize_storage_path(void) {
    char tmp[PATH_MAX + sizeof(bwfs.storage_path)];
    if (bwfs.storage_path[0] == '/') {
        // Ruta absoluta: solo limpieza
        compact_slashes(bwfs.storage_path, tmp, sizeof(tmp));
        strncpy(bwfs.storage_path, tmp, sizeof(bwfs.storage_path) - 1);
        bwfs.storage_path[sizeof(bwfs.storage_path) - 1] = '\0';
        return;
    }
    // Ruta relativa: convertir a absoluta con cwd
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, bwfs.storage_path);
        char compacted[PATH_MAX + sizeof(bwfs.storage_path)];
        compact_slashes(tmp, compacted, sizeof(compacted));
        strncpy(bwfs.storage_path, compacted, sizeof(bwfs.storage_path) - 1);
        bwfs.storage_path[sizeof(bwfs.storage_path) - 1] = '\0';
    }
}

// Verifica que el directorio de storage sea válido; si no existe, lo crea.
// Retorna 0 si OK, negativo si error (ENOTDIR, EIO, etc.)
static int ensure_storage_dir_exists(void) {
    // Guardas contra rutas corruptas ("/" o empezando con "//")
    if (strcmp(bwfs.storage_path, "/") == 0 || strncmp(bwfs.storage_path, "//", 2) == 0) {
        fprintf(stderr, "[bwfs] storage_path inválido: '%s'\n", bwfs.storage_path);
        return -ENOTDIR;
    }
    struct stat st;
    if (stat(bwfs.storage_path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0; // Ya existe y es directorio
        fprintf(stderr, "[bwfs] storage_path existe pero no es directorio: %s\n", bwfs.storage_path);
        return -ENOTDIR;
    }
    // No existe: intentar crear
    if (mkdir(bwfs.storage_path, 0755) != 0) {
        perror("[bwfs] mkdir storage_path");
        return -EIO;
    }
    return 0;
}
// Carga y guardado de metadata persistente (bwfs_metadata.bin)
// Carga la metadata desde META_FILENAME al estado en memoria (bwfs)
// - Valida fingerprint (magic)
// - Copia storage_path y max_block_bytes
// - Inicializa inodos en RAM con valores POSIX (uid/gid/tiempos)
static int load_metadata(void) {
    bwfs_disk_t disk;
    char meta_path[PATH_MAX];

     // Resolver ruta absoluta de metadata (defensivo)
    if (!realpath(META_FILENAME, meta_path)) {
        fprintf(stderr, "[bwfs] no se pudo resolver ruta de %s\n", META_FILENAME);
        return -1;
    }

    FILE *fp = fopen(meta_path, "rb");
    if (!fp) {
        fprintf(stderr, "[bwfs] no se pudo abrir %s\n", meta_path);
        return -1;
    }

    size_t r = fread(&disk, sizeof(disk), 1, fp);
    fclose(fp);
    if (r != 1) {
        fprintf(stderr, "[bwfs] metadata incompleta\n");
        return -1;
    }
    // Debug: fingerprint y storage en crudo
    fprintf(stderr, "[DEBUG] disk.magic=0x%08x\n", disk.magic);
    fprintf(stderr, "[DEBUG] disk.storage_path(raw)='%s'\n", disk.storage_path);
    // Validación de fingerprint
    if (disk.magic != BWFS_MAGIC) {
        fprintf(stderr, "[bwfs] fingerprint inválido en metadata (magic=%08x)\n", disk.magic);
        return -1;
    }
    // Copiar parámetros al estado en memoria
    strncpy(bwfs.storage_path, disk.storage_path, sizeof(bwfs.storage_path) - 1);
    bwfs.storage_path[sizeof(bwfs.storage_path) - 1] = '\0';
    bwfs.max_block_bytes = disk.max_block_bytes;
    // Normalizar ruta
    normalize_storage_path();

    fprintf(stderr, "[DEBUG] storage_path(normalized)='%s'\n", bwfs.storage_path);
    // Inicializar tabla de inodos en RAM (con atributos POSIX)
    for (int i = 0; i < MAX_FILES; i++) {
        bwfs.inodes[i].used = disk.inodes[i].used;
        strncpy(bwfs.inodes[i].name, disk.inodes[i].name, MAX_NAME_LEN);
        bwfs.inodes[i].name[MAX_NAME_LEN] = '\0';
        bwfs.inodes[i].mode = (disk.inodes[i].used ? (S_IFREG | 0644) : 0);
        bwfs.inodes[i].uid = getuid();
        bwfs.inodes[i].gid = getgid();
        bwfs.inodes[i].size = disk.inodes[i].size;
        time_t now = time(NULL);
        bwfs.inodes[i].atime = now;
        bwfs.inodes[i].mtime = now;
        bwfs.inodes[i].ctime = now;
    }
    return 0;
}

// Persiste la metadata actual (bwfs, bwfs_metadata.bin)
// - Escribe fingerprint, storage_path, max_block_bytes e inodos
static int save_metadata(void) {
    bwfs_disk_t disk;
    memset(&disk, 0, sizeof(disk));
    disk.magic = BWFS_MAGIC;
    strncpy(disk.storage_path, bwfs.storage_path, sizeof(disk.storage_path) - 1);
    disk.max_block_bytes = bwfs.max_block_bytes;
    for (int i = 0; i < MAX_FILES; i++) {
        disk.inodes[i].used = bwfs.inodes[i].used;
        strncpy(disk.inodes[i].name, bwfs.inodes[i].name, sizeof(disk.inodes[i].name) - 1);
        disk.inodes[i].size = bwfs.inodes[i].size;
        disk.inodes[i].mode = bwfs.inodes[i].mode;
    }

    FILE *fp = fopen(META_FILENAME, "wb");
    if (!fp) return -1;
    size_t w = fwrite(&disk, sizeof(disk), 1, fp);
    fclose(fp);
    return (w == 1) ? 0 : -1;
}

// Inicialización del FS en FUSE (hook .init)

// Se llama cuando FUSE monta el FS.
// - Intenta cargar metadata. Si falla, inicializa con defaults.
// - Normaliza rutas, asegura directorio de storage y persiste defaults.
// - Activa kernel_cache para optimizar atributos.

static void *bwfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    (void)conn;
    cfg->kernel_cache = 1; // Cache de atributos y nombres en el kernel (mejor rendimiento)

    if (load_metadata() != 0) {
        fprintf(stderr, "[bwfs_init] metadata no válida, usando defaults\n");
        memset(&bwfs, 0, sizeof(bwfs));
        strncpy(bwfs.storage_path, "bwfs_storage", sizeof(bwfs.storage_path) - 1);
        bwfs.storage_path[sizeof(bwfs.storage_path) - 1] = '\0';
        bwfs.max_block_bytes = 1000000;
        normalize_storage_path();
        (void)ensure_storage_dir_exists();
        (void)save_metadata();
    }

    fprintf(stderr, "[bwfs_init] storage_path='%s'\n", bwfs.storage_path);
    return NULL; // No usamos un contexto propio adicional
}

// Utilidades de nombres e inodos (lookup y asignación)
static const char *basename_from_path(const char *path) {
    return (path[0] == '/') ? path + 1 : path;
}
// Busca un inodo por nombre de archivo. Retorna índice o -1 si no está.
static int find_inode_by_name(const char *path) {
    const char *fname = basename_from_path(path);
    if (!fname || !fname[0]) return -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (bwfs.inodes[i].used && strcmp(bwfs.inodes[i].name, fname) == 0)
            return i;
    }
    return -1;
}

// Reserva un inodo libre y crea su backing file físico en storage.
// - Valida nombre (no vacío, longitud, no duplicado)
// - Asegura existencia del directorio de storage
// - Inicializa atributos POSIX (modo, uid/gid, tiempos)
// - Crea/trunca el archivo físico
// - Persiste metadata
// Retorna índice del inodo asignado o código de error negativo.
static int allocate_inode(const char *path, mode_t mode) {
    const char *fname = basename_from_path(path);
    if (!fname || !fname[0]) return -EINVAL; // Nombre vacío no válido
    if (strlen(fname) > MAX_NAME_LEN) return -ENAMETOOLONG; // Nombre demasiado largo
    if (find_inode_by_name(path) >= 0) return -EEXIST; // Ya existe

    // Asegura directorio de storage válido antes de crear backing file
    if (ensure_storage_dir_exists() != 0) {
        return -EIO;
    }

    for (int i = 0; i < MAX_FILES; ++i) {
        // Marcar inodo como usado e inicializar metadata
        if (!bwfs.inodes[i].used) {
            bwfs.inodes[i].used = 1;
            strncpy(bwfs.inodes[i].name, fname, MAX_NAME_LEN);
            bwfs.inodes[i].name[MAX_NAME_LEN] = '\0';
            bwfs.inodes[i].mode = S_IFREG | (mode ? (mode & 0777) : 0644);
            bwfs.inodes[i].uid = getuid();
            bwfs.inodes[i].gid = getgid();
            bwfs.inodes[i].size = 0;
            time_t now = time(NULL);
            bwfs.inodes[i].atime = bwfs.inodes[i].mtime = bwfs.inodes[i].ctime = now;

            // Prepara ruta del backing file y crear/truncar 
            char filepath[PATH_MAX];
            snprintf(filepath, sizeof(filepath), "%s/file_%04d.dat", bwfs.storage_path, i);
            int fd = open(filepath, O_CREAT | O_TRUNC | O_WRONLY, 0644);
            if (fd < 0) {
                perror("[bwfs] open backing file");
                // Rollback si falló la creación del archivo físico
                bwfs.inodes[i].used = 0;
                bwfs.inodes[i].name[0] = '\0';
                return -EIO;
            }
            close(fd);
            // Persistir tabla de inodos y parámetros
            (void)save_metadata();
            return i; // Índice del inodo asignado
        }
    }
    return -ENOSPC; // No hay inodos libres
}


/**
 *  Funciones principales de operaciones FUSE
 * 
 */


// 1. Función getattr

static int bwfs_getattr(const char *path, struct stat *stbuf,
                        struct fuse_file_info *fi) {
    (void) fi; // No usamos fi en esta implementación
    memset(stbuf, 0, sizeof(struct stat)); // Inicializa struct stat a cero
    
    // 1) Atributos del directorio raíz "/"
    // FUSE necesita saber que "/" es un directorio (S_IFDIR) con permisos 0755.
    // st_nlink = 2 es el estándar para directorios (self y parent).
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

     // 2) Atributos de archivos regulares en el FS
    // Se busca el inodo cuyo nombre coincide con path+1 (sin el prefijo "/").
    // Si existe, se llenan los atributos POSIX: modo, tamaño, propietario, tiempos.
    for (int i = 0; i < MAX_FILES; i++) {
        if (bwfs.inodes[i].used && strcmp(path+1, bwfs.inodes[i].name) == 0) {
            stbuf->st_mode = bwfs.inodes[i].mode;  // S_IFREG | permisos (0644, etc.)
            stbuf->st_nlink = 1;                     // Archivos regulares tienen 1 enlace
            stbuf->st_size = bwfs.inodes[i].size;  // Tamaño lógico
            stbuf->st_uid = bwfs.inodes[i].uid;  // Propietario
            stbuf->st_gid = bwfs.inodes[i].gid;  // Grupo
            stbuf->st_atime = bwfs.inodes[i].atime;  // Último acceso
            stbuf->st_mtime = bwfs.inodes[i].mtime;  // Última modificación de contenido
            stbuf->st_ctime = bwfs.inodes[i].ctime;  // Último cambio de metadata
            return 0;   
    }
    // 3) Si no se encuentra, FUSE reporta "no existe" (ENOENT)
    return -ENOENT;
}
}


// 2. Función readdir
static int bwfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi,
                        enum fuse_readdir_flags flags) {
    (void) offset; (void) fi; (void) flags;
    
    // 1) Solo soportamos listar el directorio raíz "/"
    // Si piden otro path, se reporta "no existe".
    if (strcmp(path, "/") != 0)
        return -ENOENT;
    // 2) Entradas estándar en un directorio: "." y ".."
    // FUSE requiere añadirlas manualmente.                       
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    // 3) Listar todos los archivos (inodos usados) como entradas del directorio
    // Cada inodo usado expone su nombre en el listado.                        
    for (int i = 0; i < MAX_FILES; i++) {
        if (bwfs.inodes[i].used)
            filler(buf, bwfs.inodes[i].name, NULL, 0, 0);
    }

    return 0;
}

// 3. Función create
static int bwfs_create(const char *path, mode_t mode,
                       struct fuse_file_info *fi) {
    (void) fi;
    // 1) Crea un nuevo archivo lógico en el FS
    // Busca un inodo libre, inicializa metadata POSIX, y persiste metadata.
    // Nota: Esta versión mínima NO escribe contenido en backing files aquí
    for (int i = 0; i < MAX_FILES; i++) {
        if (!bwfs.inodes[i].used) {
            bwfs.inodes[i].used = 1;
            // path es "/nombre"; almacenamos sin la barra inicial
            strncpy(bwfs.inodes[i].name, path+1, MAX_NAME_LEN);
            bwfs.inodes[i].mode = S_IFREG | mode; // Archivo regular + permisos solicitados
            bwfs.inodes[i].uid = getuid();
            bwfs.inodes[i].gid = getgid();
            bwfs.inodes[i].size = 0;
            time_t now = time(NULL);
            bwfs.inodes[i].atime = now;
            bwfs.inodes[i].mtime = now;
            bwfs.inodes[i].ctime = now;
            // Persistir cambios en bwfs_metadata.bin (tabla de inodos)
            save_metadata();
            return 0;
        }
    }
    // 2) Si no hay inodos libres, reporta "sin espacio" (ENOSPC)
    return -ENOSPC;
}


// 4. Funcion unlink
static int bwfs_unlink(const char *path) {
    // 1) Borrar archivo lógico del FS (no directorios en esta versión)
    // Busca el inodo por nombre; si existe, lo marca libre y limpia campos
    for (int i = 0; i < MAX_FILES; i++) {
        if (bwfs.inodes[i].used && strcmp(path+1, bwfs.inodes[i].name) == 0) {
            bwfs.inodes[i].used = 0;
            bwfs.inodes[i].name[0] = '\0';
            bwfs.inodes[i].size = 0;
            bwfs.inodes[i].mode = 0;
            // Persistir tabla de inodos tras el borrado
            save_metadata();
            // 2) Si no se encuentra, reporta "no existe" (ENOENT)
            return 0;
        }
    }
    return -ENOENT;
}



// Función extra utimens que ayuda para evitar errores en manejos
static int bwfs_utimens(const char *path, const struct timespec tv[2],
                        struct fuse_file_info *fi) {
    (void) fi;
    // 1) Actualizar tiempos (atime, mtime) del archivo solicitado
    // touch y otras herramientas POSIX invocan esta operación.
    for (int i = 0; i < MAX_FILES; i++) {
        if (bwfs.inodes[i].used && strcmp(path+1, bwfs.inodes[i].name) == 0) {
            // tv[0] = atime, tv[1] = mtime (en segundos desde epoch)
            bwfs.inodes[i].atime = tv[0].tv_sec;
            bwfs.inodes[i].mtime = tv[1].tv_sec;
            // Persistir cambios en metadata
            save_metadata();
            return 0;
        }
    }
    // 2) Si el archivo no existe, ENOENT
    return -ENOENT;
}


/* ---- Tabla de operaciones FUSE ----
   Esta estructura enlaza las funciones implementadas arriba con FUSE.
   El kernel llamará estas funciones cuando el usuario use comandos como
   ls, touch, rm, stat, etc. en el directorio de montaje.
*/
static struct fuse_operations bwfs_oper = {
    .init       = bwfs_init, // Inicializa el FS al montar (carga metadata y rutas)
    .getattr    = bwfs_getattr, // Atributos: ls -l, stat
    .readdir    = bwfs_readdir, // Listado de directorio: ls
    .create     = bwfs_create, // Crear archivo: touch
    .unlink     = bwfs_unlink, // Borrar archivo: rm
    .utimens    = bwfs_utimens, // Actualizar tiempos: touch actualiza atime/mtime
};

/**
 *   Main
 * 
 *  Bootstrap del FS: procesa un posible "-c config.ini" para compatibilidad de CLI,
 *  luego transfiere el control a FUSE mediante fuse_main(), que montará el FS
*   y empezará a invocar las funciones registrados en bwfs_oper.
 * 
 */
int main(int argc, char *argv[]) {
    /* Compatibilidad con CLI: permitimos "-c config.ini" aunque
       bwfs realmente usa bwfs_metadata.bin para su configuración final.
       Aquí solo reportamos que se recibió y ajustamos argc/argv para FUSE. */
    const char *config_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            config_file = argv[i + 1];
            // Compactar argv eliminando "-c <file>" para que FUSE no lo procese
            for (int j = i + 2; j < argc; j++) argv[j - 2] = argv[j];
            argc -= 2;
            break;
        }
    }
    if (config_file) {
        printf("Usando configuración: %s\n", config_file);
        /* Nota: mkfs.bwfs ya persistió la configuración en bwfs_metadata.bin.
           En bwfs.c, load_metadata() lee ese archivo y establece storage_path,
           max_block_bytes e inodos. El argumento -c es informativo aquí. */
    }
    // Entrar al loop principal de FUSE:
    // - Monta el FS en el punto de montaje recibido en argv (p.e., "mnt")
    // - Llama a bwfs_init()
    // - A partir de ahí, cada operación del usuario se despacha a bwfs_oper.*
    return fuse_main(argc, argv, &bwfs_oper, NULL);
}

