#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define META_FILE "bwfs_metadata.bin" // Archivo principal de metadata del sistema BWFS
#define MAX_FILES 1024 // Número máximo de bloques/archivos permitidos
#define STORAGE_DIR "bwfs_storage" // Directorio donde se almacenan los .dat
#define IMG_DIR "storage_images"  // Carpeta donde se exportarán las imágenes PBM

// Carga un archivo completo en memoria
// path: ruta del archivo a leer
// out_size: apuntador donde se guarda el tamaño leído
// Devuelve: buffer con el archivo cargado o NULL si falla
static unsigned char *load_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb"); // Abre un archivo en modo binario
    if (!f) return NULL; 

    fseek(f, 0, SEEK_END); // Mueve el puntero al final del archivo para determinar tamaño
    long s = ftell(f); // Obtiene el tamaño del archivo
    fseek(f, 0, SEEK_SET); // regresa al inicio del archivo

    unsigned char *buf = malloc(s);  // Reserva en memoria
    fread(buf, 1, s, f); // Lee el contenido del buffer
    fclose(f);

    *out_size = s;
    return buf;
}

// Convierte un buffer binario a una imagen PBM (formato blanco/negro)
// filename: ruta del archivo PBM de salida
// data: buffer binario que queremos representar
// size: tamaño del buffer
static void buffer_to_pbm(const char *filename, unsigned char *data, size_t size)
{
    int side = 1;
    // Calcula tamaño de la imagen cuadrada mínima que pueda contener todos los bytes
    while (side * side < size) side++;

    FILE *f = fopen(filename, "wb"); // Abrir PBM para escritura binaria
    if (!f)
    {
        printf("[ERROR] No se pudo crear %s\n", filename);
        return;
    }

    // Formato PBM tipo P4: imagen monocromática (1 bit por pixel usualmente)
    fprintf(f, "P4\n%d %d\n", side, side);

    // Recorre todos los píxeles de la imagen cuadrada
    for (size_t i = 0; i < (size_t)(side * side); i++)
    {
        // Tomar el byte correspondiente, si no existe poner 0
        unsigned char byte = (i < size ? data[i] : 0);
        // Si el byte es >=128, pixel blanco; si no, negro
        unsigned char pixel = (byte >= 128 ? 0xFF : 0x00);
        fwrite(&pixel, 1, 1, f); // Escribe el pixel en el archivo
    }

    fclose(f);
}

// Crea un directorio si no existe
// dir: nombre del directorio a asegurar
static void ensure_dir(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) == -1)
    {
        if (mkdir(dir, 0777) != 0)
        {
            perror("mkdir");
            exit(1);
        }
    }
}

int main()
{
    printf("[bwfs_export] Exportando imágenes a '%s/'…\n", IMG_DIR);

    ensure_dir(IMG_DIR); // Crea el directorio de salida si no existe

    // Exporta metadata como PBM
    size_t meta_size = 0;
    unsigned char *meta = load_file(META_FILE, &meta_size);

    if (!meta)
    {
        printf("[ERROR] No se pudo leer metadata\n");
        return 1;
    }

    char meta_out[256];
    snprintf(meta_out, sizeof(meta_out), "%s/metadata.pbm", IMG_DIR);
    buffer_to_pbm(meta_out, meta, meta_size);
    free(meta);

    /* Exporta todos los bloques */
    for (int i = 0; i < MAX_FILES; i++)
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/file_%04d.dat", STORAGE_DIR, i);

        size_t size = 0;
        unsigned char *buf = load_file(path, &size);

        if (!buf) continue; // Si no existe, lo saltamos

        char outname[256];
        snprintf(outname, sizeof(outname), "%s/block_%04d.pbm", IMG_DIR, i);

        buffer_to_pbm(outname, buf, size);
        free(buf);
    }

    printf("[bwfs_export] Exportación finalizada correctamente.\n");
    return 0;
}
