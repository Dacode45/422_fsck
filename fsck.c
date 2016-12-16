#include <assert.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>


#include "fs.h"

/*********************
File Types
*********************/
#define T_DIR  1   // Directory
#define T_FILE 2   // File
#define T_DEV  3   // Device

/**********************
CONVINIENCE MACROS
**********************/
#define GET_SUPER_BLOCK(file_ptr) (struct superblock*)((char*)file_ptr + BSIZE)
#define GET_INODE(i, sb, file_ptr) ((struct dinode*)((char*)file_ptr + (sb->inodestart)*BSIZE)+i)
#define GET_BLOCK(i, file_ptr) ((void*)((char *)file_ptr + (i * BSIZE)))
//TODO : Is it big endian or little endian 76543210 or 01234567
#define IS_BLOCK_ALLOC(i, sb, file_ptr) ((*((char*)file_ptr + (sb->bmapstart)*BSIZE + (i/8))) & (1 << (i%8)))
#define DATA_START(sb, file_ptr) ((void*)((char*)file_ptr + (sb->size - sb->nblocks)*BSIZE))
/************************
GLOBALS
************************/
void* file_ptr; //mmaped file system file
struct superblock *sb; //superblock

// 0 indicates unallocated
// 1 indicates allocated
// >=2 indicates allocated and referenced.
short *inode_refs; // reference count of inodes
short *in_use_blocks; // whether this block has been marked in use previously


int debugf(char const *fmt, ...);
int checkDir(ushort current, ushort parent);
int checkFile(ushort current, ushort parent);
int checkDev(ushort current, ushort parent);
int main(int argc, char **argv) {

  assert(sizeof(int) == 4);

  if(argc < 2){
    fprintf(stderr, "Usage: fsck fs.img\n");
    exit(1);
  }
  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  struct stat file_stat;

  int fd = open(argv[1], O_RDWR);
  if (fd < 0) {
    fprintf(stderr,"image not found.\n");
    exit(1);
  }
  fstat(fd, &file_stat);
  int file_size = file_stat.st_size;

  file_ptr = mmap(NULL, file_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
  sb = GET_SUPER_BLOCK(file_ptr);
  debugf("Superblock:\n"
    "size: %d,"
    "nblocks: %d,"
    "ninodes: %d,"
    "nlog: %d,"
    "logstart: %d,"
    "inodestart: %d,"
    "bmapstart %d\n", sb->size, sb->nblocks, sb->ninodes, sb->nlog, sb->logstart, sb->inodestart, sb->bmapstart);

    //SIZE CONSISTENCY CHECKS
    if (sb->size*BSIZE != (uint)file_size) {
      fprintf(stderr,"Superblock size does not match file size");
      return 1;
    }
    if (sb->nblocks > sb->size) {
      fprintf(stderr,"Block list is greater than file system size");
      return 1;
    }

    inode_refs = (short *)calloc(sb->ninodes, sizeof(short));
    in_use_blocks = (short*)calloc(sb->size, sizeof(short));

    // check all allocated inodes
    uint i;
    debugf("file_ptr: %p, super: %p\n", file_ptr, sb);
    for(i = (sb->size - sb->nblocks); i < sb->size; i++) {
      debugf("bitmap for block %d = %x\n", i, 0xff &  (*((char*)file_ptr + (sb->bmapstart)*BSIZE + i/8)));
    }
    for(i = 1; i < sb->ninodes; i++) {

      struct dinode* inode = GET_INODE(i, sb, file_ptr);
      debugf("inode %i, type: %d, size: %d, first: %d\n",i,  inode->type, inode->size, inode->addrs[0]);
      if (inode->type == 0) {
        continue;
      }
      if (inode->type == T_FILE || inode->type == T_DIR || inode->type == T_DEV) {
        // increase referense count by 1
        inode_refs[i]++;
      } else {
        fprintf(stderr,"bad inode\n");
        return 1;
      }
    }

    // Check root
    struct dinode *root_node = GET_INODE(1, sb, file_ptr);
    if (root_node->type != T_DIR) {
      fprintf(stderr,"root directory does not exist.\n");
      return 1;
    }
    checkDir(1, 1);

    // Check hard links
    for(i = 2; i < sb->ninodes; i++) {
      struct dinode *inode = GET_INODE(i, sb, file_ptr);
      short expected_ref_count = inode->nlink;
      short actual_ref_count = inode_refs[i] - 1;

      // inode unallocated
      if (inode_refs[i] == 0) {
        continue;
      }
      if (inode_refs[i] == 1) {
        fprintf(stderr,"inode marked use but not found in a directory");
        return 1;
      }
      // Directories should only have 1 reference
      if (inode->type == T_DIR) {
        if (actual_ref_count != 1) {
          fprintf(stderr,"directory appears more than once in file system.\n");
          return 1;
        }
      }else if (inode->type == T_FILE){
        if (expected_ref_count != actual_ref_count) {
          fprintf(stderr,"bad reference count for file");
          return 1;
        }
      }
    }

    // Check data blocks are in use.
    int starting_data_block = sb->size - sb->nblocks;
    for(i = starting_data_block; i < sb->size; i++) {
      if (in_use_blocks[i] != 0 && !IS_BLOCK_ALLOC(i, sb, file_ptr)) {
        fprintf(stderr,"address used by inode but marked free in bitmap.\n");
        exit(1);
      }
      if (in_use_blocks[i] == 0 && IS_BLOCK_ALLOC(i, sb, file_ptr)) {
        fprintf(stderr,"bitmap marks block in use but it its not in use.\n");
        exit(1);
      }
    }
}

int checkDir(ushort current, ushort parent) {
  struct dinode *c_inode = GET_INODE(current, sb, file_ptr);
  uint has_dot = 0;
  uint has_dot_dot = 0;
  uint has_indirect = 0;

  if (c_inode->type != T_DIR) { // not a directory.
    return 0;
  }

  // Go through each direntry
  uint db_count; // Number of datablocks inode uses
  uint dirent_count = 0; // Number of dirent inode uses

  // Go through each dirent;
  for (db_count = 0; (db_count >= NDIRECT && db_count < MAXFILE)  || c_inode->addrs[db_count] != 0; db_count++) {

    // get the given block
    void* block;
    if (db_count < NDIRECT) {
      debugf("Geting direct block %d @ %d for inode: %d\n", db_count,c_inode->addrs[db_count], current);
      block = GET_BLOCK(c_inode->addrs[db_count], file_ptr);
      if(in_use_blocks[c_inode->addrs[db_count]] != 0) {
        fprintf(stderr,"address used more than once.\n");
        exit(1);
      }
      in_use_blocks[db_count]++;
    } else {
      // first check indirect block is not null
      if (c_inode->addrs[NDIRECT] == 0) {
        break;
      }
      block = GET_BLOCK(c_inode->addrs[NDIRECT], file_ptr);
      // Check block is in the data partition
      if (block < DATA_START(sb,file_ptr) ) {
        fprintf(stderr,"bad address in inode.\n");
        exit(1);
      }
      if (!has_indirect && in_use_blocks[NDIRECT] != 0) {
        fprintf(stderr,"address used more than once.\n");
        exit(1);
      }
      has_indirect = 1;

      // Get the indirect block and check that it is not 0
      uint * indirect_blocks = (uint*)block;
      if (indirect_blocks[db_count-NDIRECT] == 0) {
        break;
      }
      debugf("Geting indirect block %d @ (%d->%d) for inode: %d\n", db_count, c_inode->addrs[NDIRECT], indirect_blocks[db_count-NDIRECT], current);
      block = GET_BLOCK(indirect_blocks[db_count-NDIRECT], file_ptr);
      if(in_use_blocks[db_count-NDIRECT] != 0) {
        fprintf(stderr,"address used more than once\n");
        exit(1);
      }
      in_use_blocks[db_count-NDIRECT]++;
    }

    // Check block is in the data partition
    if (block < DATA_START(sb,file_ptr) ) {
      fprintf(stderr,"bad address in inode\n");
      exit(1);
    }

    // Check block is allocated
    if (!IS_BLOCK_ALLOC(c_inode->addrs[db_count], sb, file_ptr)){
      fprintf(stderr,"here address used by inode but marked free in bitmap\n");
      exit(1);
    }

    struct dirent* tmp_dirent;
    // Go through all dirent
    for(tmp_dirent = (struct dirent*)block; tmp_dirent->inum != 0 && (char*)tmp_dirent < (char*)block + BSIZE; tmp_dirent++) {
      dirent_count++;

      // Check "." and ".."
        if (strcmp(tmp_dirent->name, ".") == 0) {
          has_dot++;
          debugf("\tdirectory inode: %d, has \".\"\n", current);
          // check that it points to root.
          if (current == 1 && (tmp_dirent->inum != 1)) {
            fprintf(stderr,"root directory does not exist");
            exit(1);
          }
          continue;
        }else if(strcmp(tmp_dirent->name, "..") == 0) {
          has_dot_dot++;
          debugf("\tdirectory inode: %d, has \"..\"\n", current);
          if (current == 1 && (tmp_dirent->inum != 1)) {
            fprintf(stderr,"root directory does not exist");
            exit(1);
          }if (tmp_dirent->inum != parent) {
            fprintf(stderr,"parent directory mismatch");
            exit(1);
          }
          continue;
        }

      ushort tmp_inode_num = tmp_dirent->inum;
      struct dinode* tmp_inode = GET_INODE(tmp_inode_num, sb,file_ptr);

      // Increase the referece to this inode
      if (inode_refs[tmp_inode_num] == 0) {
        fprintf(stderr,"inode referred to in directory but marked free.\n");
        exit(1);
      }
      inode_refs[tmp_inode_num]++;

      // Recurse
      debugf("recursing from %d to %d:%s\n", current, tmp_inode_num, tmp_dirent->name);
      if (tmp_inode->type == T_DIR) {
        checkDir(tmp_inode_num, current);
      } else if (tmp_inode->type == T_FILE) {
        checkFile(tmp_inode_num, current);
      } else if (tmp_inode->type == T_DEV) {
        checkFile(tmp_inode_num, current);
      } else if (tmp_inode->type == 0) {
        fprintf(stderr,"inode refered to in directory but marked free.\n");
        exit(1);
      } else {
        fprintf(stderr,"bad inode");
        exit(1);
      }
    }

  }

  if(has_dot != 1) {
    fprintf(stderr,"directory not properly formatted.\n");
    exit(1);
  }
  if (has_dot_dot != 1) {
    fprintf(stderr,"directory not properly formatted.\n");
    exit(1);
  }
  return 0;
}

int checkFile(ushort current, ushort parent) {
    struct dinode *c_inode = GET_INODE(current, sb, file_ptr);

    if (c_inode->type != T_DIR) { // not a directory.
      return 0;
    }

    // Go through each direntry
    uint db_count; // Number of datablocks inode uses
    uint has_indirect = 0;
    // Go through each dirent;
    for (db_count = 0; (db_count >= NDIRECT && db_count < MAXFILE)  || c_inode->addrs[db_count] != 0; db_count++) {
      debugf("Checking directory inode: %d, datablock: %d\n", current, db_count);

      // get the given block
      void* block;
      if (db_count < NDIRECT) {
        debugf("Geting direct block %d @ %d for inode: %d", db_count,c_inode->addrs[db_count], current);
        block = GET_BLOCK(c_inode->addrs[db_count], file_ptr);
        if(in_use_blocks[c_inode->addrs[db_count]] != 0) {
          fprintf(stderr,"address used more than once");
          exit(1);
        }
        in_use_blocks[db_count]++;
      } else {
        // first check indirect block is not null
        if (c_inode->addrs[NDIRECT] == 0) {
          break;
        }
        block = GET_BLOCK(c_inode->addrs[NDIRECT], file_ptr);
        // Check block is in the data partition
        if (block < DATA_START(sb,file_ptr) ) {
          fprintf(stderr,"bad address in inode");
          exit(1);
        }
        if (!has_indirect && in_use_blocks[NDIRECT] != 0) {
          fprintf(stderr,"address used more than once");
          exit(1);
        }
        has_indirect = 1;

        // Get the indirect block and check that it is not 0
        uint * indirect_blocks = (uint*)block;
        if (indirect_blocks[db_count-NDIRECT] == 0) {
          break;
        }
        debugf("Geting indirect block %d @ (%d->%d) for inode: %d", db_count, c_inode->addrs[NDIRECT], indirect_blocks[db_count-NDIRECT], current);
        block = GET_BLOCK(indirect_blocks[db_count-NDIRECT], file_ptr);
        if(in_use_blocks[db_count-NDIRECT] != 0) {
          fprintf(stderr,"address used more than once");
          exit(1);
        }
        in_use_blocks[db_count-NDIRECT]++;
      }

      // Check block is in the data partition
      if (block < DATA_START(sb,file_ptr) ) {
        fprintf(stderr,"bad address in inode");
        exit(1);
      }

      // Check block is allocated
      if (IS_BLOCK_ALLOC(c_inode->addrs[db_count], sb, file_ptr)){
        fprintf(stderr,"address used by inode but marked free in bitmap\n");
        exit(1);
      }
    }

    if (c_inode->size > (db_count+1)*BSIZE) {
      fprintf(stderr,"inode size and allocated memory do not match up\n");
      exit(1);
    }

    return 0;
  }

int debugf(char const *fmt, ...) {
  int ret = 0;

  #ifdef DEBUG
    va_list myargs;
    va_start(myargs, fmt);
    ret = vprintf(fmt, myargs);
    va_end(myargs);
  #endif
  return ret;
}
