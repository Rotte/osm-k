/*
 * FlatFS header file for OSM exam at DIKU 2012.
 *
 * This file specifies the interface of the FlatFS file system driver
 * as described in the assignment.
 *
 * @author Søren Dahlgaard
 */

#ifndef FS_FLATFS_H
#define FS_FLATFS_H

#include "drivers/gbd.h"
#include "fs/vfs.h"
#include "lib/libc.h"
#include "lib/bitmap.h"

/* In FLATFS block size is 512. This will affect to various other
   features of FLATFS e.g. maximum file size. */
#define FLATFS_BLOCK_SIZE 512

/* Magic number found on each flatfs filesystem's header block. */
#define FLATFS_MAGIC 0x00BAB5E2

/* Block numbers for system blocks */
#define FLATFS_HEADER_BLOCK 0
#define FLATFS_ALLOCATION_BLOCK 1
#define FLATFS_DIRECTORY_BLOCK  2

/* Names are limited to 16 characters */
#define FLATFS_VOLUMENAME_MAX 16
#define FLATFS_FILENAME_MAX 16

/* Maximum number of direct/single/double indirect blocks */
#define FLATFS_BLOCKS_DIRECT 7

#define FLATFS_BLOCKS_SINDIR (FLATFS_BLOCK_SIZE / sizeof(uint32_t))
#define FLATFS_BLOCKS_DINDIR (FLATFS_BLOCKS_SINDIR * FLATFS_BLOCKS_SINDIR)

/* Maximum amount of blocks for a file */
#define FLATFS_MAX_BLOCKS \
    (FLATFS_BLOCKS_DIRECT + FLATFS_BLOCKS_SINDIR + FLATFS_BLOCKS_DINDIR)

/* File inode block. Inode contains the filesize and a table of blocknumbers
   allocated for the file. In TFS files can't have more blocks than fits in
   block table of the inode block. 

   One 512 byte block can hold 128 32-bit integers. Therefore the table
   size is limited to 127 and filesize to 127*512=65024.
   */

typedef struct {
    /* filesize in bytes */
    uint32_t filesize;

    /* Data blocks. Some are not direct. */
    uint32_t block[FLATFS_BLOCKS_DIRECT];
    uint32_t block_indir;
    uint32_t block_dbl_indir;
} flatfs_inode_t;


/* Master directory block entry. If inode is zero, entry is 
   unused (free). */
typedef struct {
    /* File's inode block number. */
    uint32_t inode;

    /* File name */
    char     name[FLATFS_FILENAME_MAX];
} flatfs_direntry_t;

#define FLATFS_MAX_FILES (FLATFS_BLOCK_SIZE/sizeof(flatfs_direntry_t))

/* functions */
fs_t * flatfs_init(gbd_t *disk);

int flatfs_unmount(fs_t *fs);
int flatfs_open(fs_t *fs, char *filename);
int flatfs_close(fs_t *fs, int fileid);
int flatfs_create(fs_t *fs, char *filename, int size);
int flatfs_remove(fs_t *fs, char *filename);
int flatfs_read(fs_t *fs, int fileid, void *buffer, int bufsize, int offset);
int flatfs_write(fs_t *fs, int fileid, void *buffer, int datasize, int offset);
int flatfs_getfree(fs_t *fs);
int flatfs_filecount(fs_t *fs, char *dirname);
int flatfs_file(fs_t *fs, char *dirname, int idx, char *buffer);
int flatfs_mkdir(fs_t *fs, char *dirname);
int flatfs_rmdir(fs_t *fs, char *dirname);

#endif
