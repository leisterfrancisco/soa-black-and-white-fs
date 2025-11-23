/**
 * BWFS - Implementación con FUSE parte de inicialización, metadata, rutas
 * y gestión de inodos

*/

#ifndef BWFS_H
#define BWFS_H

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

#define _GNU_SOURCE
#define FUSE_USE_VERSION 31

#include "network.h"
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <fuse3/fuse.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Config / límites */
#define MAX_FILES 1024
#define MAX_NAME_LEN 255
#ifndef META_FILENAME
// Default fallback if not defined at compile time
#define META_FILENAME "bwfs_metadata.bin"
#endif
#define BWFS_MAGIC 0x42574653u /* "BWFS" */
#define DEFAULT_STORAGE "bwfs_storage"
#define DEFAULT_MAX_BLOCK_BYTES 1000000UL /* 1000 x 1000 */

/* --- estructuras en disco (persistentes) --- */
typedef struct {
  int    used;
  char   name[256];
  size_t size;
  int    mode;
} inode_disk_t;

typedef struct {
  unsigned int magic;
  char         storage_path[512];
  size_t       max_block_bytes;
  inode_disk_t inodes[MAX_FILES];
} bwfs_disk_t;

/* --- estructuras en memoria (runtime) --- */
typedef struct {
  int    used;
  char   name[MAX_NAME_LEN + 1];
  mode_t mode;
  uid_t  uid;
  gid_t  gid;
  size_t size;
  time_t atime;
  time_t mtime;
  time_t ctime;
} inode_t;

typedef struct {
  inode_t inodes[MAX_FILES];
  char    storage_path[512];
  size_t  max_block_bytes;
} bwfs_t;

void read_remote_file( const char *path );

void write_remote_file( const char *path );

void read_local_file( const char *path );

void write_local_file( const char *path );

#endif // BWFS_H
