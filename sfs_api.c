#include "sfs_api.h"
#include "disk_emu.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define BLOCK_SIZE 1024             // Block size in bytes
#define NUM_BLOCKS 1024             // Number of block in the file system
#define MAGIC 0xACBD0005            // Magic number
#define NUM_SHADOW_ROOTS 16         // Maximum number of shadow roots

#define MAX_DIRECT_PTR 14           // Maximum number of direct pointers in an i/j-node stuct

#define SUPER_BLOCK 0               // Location of superblock on disk
#define FBM_BLOCK 1                 // Location of file bit mask on disk
#define WM_BLOCK 2                  // Location of write mask on disk

#define FILENAME_SIZE 16            // Max size for the filename (includes extensions)

typedef int b_ptr_t;                // Pointer to a disk block

typedef struct _ptr_file {          // File that the indirect pointers point to
   b_ptr_t ptrs[BLOCK_SIZE/FILENAME_SIZE];
} ptr_file_t;

typedef struct _inode {
   int size;                        // Size in bytes of the file (as in the used bytes)
   b_ptr_t d_ptrs[MAX_DIRECT_PTR];  // Direct pointers
   b_ptr_t i_ptr;                   // Indirect pointer
} inode_t;

typedef struct _super_block {
   int magic;                       // Magic number
   int block_size;                  // 
   int num_blocks;                  //
   int num_inodes;                  //
   inode_t current_root;            //
   inode_t roots[NUM_SHADOW_ROOTS]; // Array of shadow roots
} super_block_t;

typedef struct fbm_t {
   char mask[NUM_BLOCKS];           // Because ints are too big
} fbm_t;

typedef struct wm_t {
   char mask[NUM_BLOCKS];
} wm_t;

void mkssfs(int fresh){
/* Need to setup:
      Super Block
         - magic
         - Block Size
         - File System Size
         - Number of i-nodes
         - Root (j-node)
         - Shadow Roots (j-node)
      

      Free BitMap
      Write Mask
      The disk!
*/
   if(fresh == 1) {              // Fresh disk -> need to perform first time setup
      if(init_fresh_disk("placeholder", BLOCK_SIZE, NUM_BLOCKS) == -1)
         exit(-1);

      // Creating superblock
      super_block_t *sb = calloc(BLOCK_SIZE, 1);   // Allocate a block to hold the superblock (1)

      sb->magic = MAGIC;
      sb->block_size = BLOCK_SIZE;
      sb->num_blocks = NUM_BLOCKS;
      // Since calloc sets everything to 0, the following are already set to 0.
//    sb->num_inodes = 0;                          // We start with no i-nodes
//    sb->current_root->size = 0;                  // Root is thus empty

      write_blocks(0, 1, sb);                      // Write the superblock
      free(sb);                                    // Free                                    (1)

      // Create FBM
      fbm_t *FBM = calloc(BLOCK_SIZE, 1);          // Allocate a whole block for the FBM      (2)

      memset(FBM->mask, 1, NUM_BLOCKS);            // Set the whole FBM to 1
      FBM->mask[SUPER_BLOCK] = 0;
      FBM->mask[FBM_BLOCK] = 0;
      FBM->mask[WM_BLOCK] = 0;                     // This is not yet used, but will be shortly!

      write_blocks(0, 1, FBM);                     // Write the FBM
      free(FBM);                                   // Free                                    (2)

      // Create WM
      wm_t *WM = calloc(BLOCK_SIZE, 1);            // Allocate a whole block for the WM       (3)

      memset(WM->mask, 1, NUM_BLOCKS);             // Set the whole WM to 1

      write_blocks(0, 1, WM);                      // Write the WM
      free(WM);                                    // Free                                    (3)
   } else {                      // Else assume it's already setup
      if(init_disk("placeholder", BLOCK_SIZE, NUM_BLOCKS) == -1)
         exit(-1);
   }
}
int ssfs_fopen(char *name){
    return 0;
}
int ssfs_fclose(int fileID){
    return 0;
}
int ssfs_frseek(int fileID, int loc){
    return 0;
}
int ssfs_fwseek(int fileID, int loc){
    return 0;
}
int ssfs_fwrite(int fileID, char *buf, int length){
    return 0;
}
int ssfs_fread(int fileID, char *buf, int length){
    return 0;
}
int ssfs_remove(char *file){
    return 0;
}
