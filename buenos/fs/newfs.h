/*
 * NewFS header file for OSM exam at DIKU 2012.
 *
 * Based on Søren Dahlgaards FlatFS implementation.  Prepared by
 * Troels Henriksen.  You may not need to modify this file.
 */

#ifndef FS_NEWFS_H
#define FS_NEWFS_H

#include "drivers/gbd.h"
#include "fs/vfs.h"
#include "lib/libc.h"
#include "lib/bitmap.h"

/* In NewFS block size is 512. */
#define NEWFS_BLOCK_SIZE 512
#define NEWFS_BLOCK_BITS (NEWFS_BLOCK_SIZE*8)

/* Magic number found on each newfs filesystem's header block. */
#define NEWFS_MAGIC 0xFEEDBEEF

/* Block numbers for system blocks */
#define NEWFS_HEADER_BLOCK 0
#define NEWFS_ALLOCATION_BLOCK 1

/* Names are limited to 16 characters */
#define NEWFS_VOLUMENAME_MAX 16
#define NEWFS_FILENAME_MAX 16

/* Maximum number of direct/single/double indirect blocks */
#define NEWFS_BLOCKS_DIRECT 7

#define NEWFS_BLOCKS_SINDIR (NEWFS_BLOCK_SIZE / sizeof(uint32_t))
#define NEWFS_BLOCKS_DINDIR (NEWFS_BLOCKS_SINDIR * NEWFS_BLOCKS_SINDIR)

/* Maximum amount of blocks for a file */
#define NEWFS_MAX_BLOCKS                                                \
  (NEWFS_BLOCKS_DIRECT + NEWFS_BLOCKS_SINDIR + NEWFS_BLOCKS_DINDIR)

/* File inode block. Inode contains the filesize and a table of blocknumbers
   allocated for the file. In TFS files can't have more blocks than fits in
   block table of the inode block. 

   One 512 byte block can hold 128 32-bit integers. Therefore the table
   size is limited to 127 and filesize to 127*512=65024.
*/

typedef struct {
  uint32_t magic;
  // == 0xFEEDBEEF
  char name[VFS_NAME_LENGTH];
  uint32_t n_blocks;
} newfs_superblock;

typedef struct {
  /* filesize in bytes */
  uint32_t filesize;
  /* Data blocks. Some are not direct. */
  uint32_t block[NEWFS_BLOCKS_DIRECT];
  uint32_t block_indir;
  uint32_t block_dbl_indir;
  uint32_t properties;
}  newfs_inode_t;

/* Master directory block entry. If inode is zero, entry is 
   unused (free). */
typedef struct {
  /* File's inode block number. */
  uint32_t inode;

  /* File name */
  char     name[NEWFS_FILENAME_MAX];
} newfs_direntry_t;

#define NEWFS_MAX_FILES (NEWFS_MAX_BLOCKS*NEWFS_BLOCK_SIZE/sizeof(newfs_direntry_t))

/* functions */
fs_t * newfs_init(gbd_t *disk);

int newfs_unmount(fs_t *fs);
int newfs_open(fs_t *fs, char *filename);
int newfs_close(fs_t *fs, int fileid);
int newfs_create(fs_t *fs, char *filename, int size);
int newfs_remove(fs_t *fs, char *filename);
int newfs_read(fs_t *fs, int fileid, void *buffer, int bufsize, int offset);
int newfs_write(fs_t *fs, int fileid, void *buffer, int datasize, int offset);
int newfs_getfree(fs_t *fs);
int newfs_filecount(fs_t *fs, char *dirname);
int newfs_file(fs_t *fs, char *dirname, int idx, char *buffer);
int newfs_mkdir(fs_t *fs, char *dirname);
int newfs_rmdir(fs_t *fs, char *dirname);

#endif
