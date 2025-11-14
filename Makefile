CC = gcc
CFLAGS = -Wall -Wextra -g `pkg-config fuse3 --cflags`
LDFLAGS = `pkg-config fuse3 --libs`

all: bwfs mkfs.bwfs mount.bwfs mnt

# bwfs es el daemon principal (contiene main)
bwfs: src/bwfs.c
	$(CC) $(CFLAGS) src/bwfs.c -o bwfs $(LDFLAGS)

# mkfs.bwfs: independiente (no enlaza bwfs.c para evitar duplicar main)
mkfs.bwfs: src/mkfs_bwfs.c
	$(CC) $(CFLAGS) src/mkfs_bwfs.c -o mkfs.bwfs

mount.bwfs: src/mount_bwfs.c
	$(CC) $(CFLAGS) src/mount_bwfs.c -o mount.bwfs

# Crear directorio mnt si no existe y ajustar permisos
mnt:
	mkdir -p mnt
	chown elias:elias mnt
	chmod 755 mnt
storage:
	mkdir -p bwfs_storage
	chown elias:elias bwfs_storage
	chmod 755 bwfs_storage
clean:
	rm -f bwfs mkfs.bwfs mount.bwfs *.o
	rm -rf bwfs_storage
	rm -f bwfs_metadata.bin
	rm -rf mnt
