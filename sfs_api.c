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
#define DEFAULT_INODE_TABLE_BLOCK 3 // Location of the very first inode table
#define DEFAULT_ROOT_DIR_BLOCK 4    // Location of root dir on disk init

#define DIR_ENTRY_SIZE 16           // Max size for a directory entry
#define FILENAME_SIZE 10            // Max size for the filename (includes extensions)

/**************************************************************************/

typedef int b_ptr_t;                // Pointer to a disk block

typedef struct _virt_addr_t {       // Actual address in the disk
   int d_ptr;                       // Direct pointer number
   int offset;                      // Offset (can be whatever you want it to be: bytes, index, ...)
} virt_addr_t;

typedef struct _ptr_file {          // File that the indirect pointers point to
   b_ptr_t ptrs[BLOCK_SIZE/sizeof(b_ptr_t)];
} ptr_file_t;

typedef struct _inode {
   int size;                        // Size in bytes of the file (as in the used bytes)
   b_ptr_t d_ptrs[MAX_DIRECT_PTR];  // Direct pointers
   b_ptr_t i_ptr;                   // Indirect pointer
} inode_t;

typedef struct _fd_t {              // File descriptor used in FDT
   inode_t inode;                   // The file's inode (not sure it's necessary; is a shortcut)
   int inode_id;                    // The id of the file's inode
   virt_addr_t read_ptr;            // Read pointer   (offset in bytes)
   virt_addr_t write_ptr;           // Write pointer  (offset in bytes)
} fd_t;

typedef struct _super_block {
   int magic;                       // Magic number
   int block_size;                  // 
   int num_blocks;                  //
   int num_inodes;                  //
   inode_t current_root;            //
   inode_t roots[NUM_SHADOW_ROOTS]; // Array of shadow roots
} super_block_t;

typedef struct _fbm_t {             // File Bit Mask
   char mask[NUM_BLOCKS];           // Because ints are too big
} fbm_t;

typedef struct _wm_t {              // Write Mask
   char mask[NUM_BLOCKS];
} wm_t;

typedef struct _dir_entry_t {       // A directory entry (default: 16 bytes)
   char filename[FILENAME_SIZE+1];  // Null terminated?
   int inode_id;
} dir_entry_t;

typedef struct _dir_t {             // A directory block (default: 64 entries)
   dir_entry_t files[BLOCK_SIZE/DIR_ENTRY_SIZE];
} dir_t;

typedef struct _inode_block_t {
   inode_t inodes[BLOCK_SIZE/sizeof(inode_t)];
} inode_block_t;

/*************************************************************************/

b_ptr_t get_unused_block();// Gets an unused block (according to some strategy)
int new_fdt_entry(inode_t, int);    // Creates a new entry in the FDT

int virt_addr_to_bytes(virt_addr_t);// Converts a virtual address it's bytes number
virt_addr_t bytes_to_virt_addr(int);// Converts a byte number to a virtual address
b_ptr_t get_block_id(inode_t*, int);// Safe conversion of pointer index to block pointer

int add_new_block(inode_t*, int, int, b_ptr_t, super_block_t*); // Adds specified block to pointed sb

/**************************************************************************/

fd_t *fdt[NUM_BLOCKS];              // File descriptor table

/**************************************************************************/

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
      Root Directory
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
      sb->num_inodes = 1;                          // We start with 1 i-node for the root dir
      sb->current_root.size = sizeof(inode_t);     // Root is thus of size inode_t ??????

      // Write current root to fdt[0]. It's a special entry, so we don't care if id is 0
      new_fdt_entry(sb->current_root, -1);

      // Creating root dir inode and placing it in first i-node block
      inode_block_t *ib = calloc(BLOCK_SIZE, 1);   // Allocate a block for the inodes         (4)
      ib->inodes[0].d_ptrs[0] = DEFAULT_ROOT_DIR_BLOCK; // inode 0 now points to root dir
      new_fdt_entry(ib->inodes[0], 0);                  // Add root dir in FDT (at index 1)
      write_blocks(DEFAULT_INODE_TABLE_BLOCK, 1, ib);   // Write inode table
      free(ib);                                    // Free                                    (4)

      sb->current_root.d_ptrs[0] = DEFAULT_INODE_TABLE_BLOCK; // Point to first inode table block

      write_blocks(SUPER_BLOCK, 1, sb);            // Write the superblock
      free(sb);                                    // Free                                    (1)

      // Create FBM
      fbm_t *FBM = calloc(BLOCK_SIZE, 1);          // Allocate a whole block for the FBM      (2)

      memset(FBM->mask, 1, NUM_BLOCKS);            // Set the whole FBM to 1
      FBM->mask[SUPER_BLOCK] = 0;
      FBM->mask[FBM_BLOCK] = 0;
      FBM->mask[WM_BLOCK] = 0;                     // This is not yet used, but will be shortly!
      FBM->mask[DEFAULT_INODE_TABLE_BLOCK] = 0;
      FBM->mask[DEFAULT_ROOT_DIR_BLOCK] = 0;

      write_blocks(FBM_BLOCK, 1, FBM);             // Write the FBM
      free(FBM);                                   // Free                                    (2)

      // Create WM
      wm_t *WM = calloc(BLOCK_SIZE, 1);            // Allocate a whole block for the WM       (3)

      memset(WM->mask, 1, NUM_BLOCKS);             // Set the whole WM to 1
      WM->mask[DEFAULT_ROOT_DIR_BLOCK] = 0;        // Set the root dir to be read-only

      write_blocks(WM_BLOCK, 1, WM);               // Write the WM
      free(WM);                                    // Free                                    (3)
   } else {                      // Else assume it's already setup
      if(init_disk("placeholder", BLOCK_SIZE, NUM_BLOCKS) == -1)
         exit(-1);
   }
}

int ssfs_fopen(char *name){
   if(name == NULL) {
      printf("[DEBUG|ssfs_open] NULL filename. Aborting\n");
      return -1;
   }

   inode_t *inode = calloc(sizeof(inode_t), 1);    // Initialize an inode                     (7)
   super_block_t *sb = malloc(BLOCK_SIZE);         // malloc                                  (5)
   read_blocks(SUPER_BLOCK, 1, sb);                // Retrieve super block

   if(strlen(name) > FILENAME_SIZE) {
      printf("[DEBUG|ssfs_open] Inputted filename too big. Truncating\n");
      name[FILENAME_SIZE] = 0;
   }

   // Check if file exists
   int inode_id = -1;                              // ID of the inode in the inode list yz6619
   virt_addr_t addr = { .d_ptr = -1, .offset = 0 };// Offset is used as the number of entries here
   int num_entries = BLOCK_SIZE/DIR_ENTRY_SIZE;
   dir_t *dir_block = malloc(BLOCK_SIZE);          // malloc                                  (6)
   b_ptr_t block_ptr = 0;

   while(addr.d_ptr*num_entries + addr.offset < sb->num_inodes) {
      addr.d_ptr++;                             // Increment block count
      addr.offset = 0;                          // Reset offset
      printf("[DEBUG|ssfs_open] Search at B: %d; O: %d\n", addr.d_ptr, addr.offset);

      if(ssfs_fread(1, dir_block, BLOCK_SIZE) == -1) { // If read fails -> end of dir file
         printf("[DEBUG|ssfs_open] File not found. At B: %d; O: %d\n", addr.d_ptr, addr.offset);
         break;
      }

      if(strcmp(name, dir_block->files[addr.offset].filename) == 0) { // Compare filename
         printf("[DEBUG|ssfs_open] File found B: %d; O: %d\n", addr.d_ptr, addr.offset);
         inode_id = addr.d_ptr*num_entries + addr.offset;
         break;
      }
      addr.offset++;
   }

   if(inode_id == -1) {                            // File does not exist
      // Need to create a file
      ssfs_fwrite(0, (char*) inode, sizeof(inode_t));// Write inode to appropriate block
   } else {                                        // File exists
      //inode = dir_block->files[addr.offset];
   }

   // Create FDT entry
   int fd = new_fdt_entry(*inode, inode_id);

   return fd;
}

int ssfs_fclose(int fileID){
   printf("[DEBUG|ssfs_fclose] Closing %d\n", fileID);
   free(fdt[fileID]);                              // Free
   fdt[fileID] = NULL;                             // Reset pointer
   return 0;
}

int ssfs_frseek(int fileID, int loc){
   if(fdt[fileID] == NULL) {
      printf("[DEBUG|ssfs_frseek] fileID %d is NULL\n", fileID);
      return -1;
   }
   if(fdt[fileID]->inode.size < loc || loc < 0) {  // Bounds checking
      printf("[DEBUG|ssfs_frseek] Out of bounds seek. loc: %d; filesize: %d\n", loc, fdt[fileID]->inode.size);
      return -1;
   }

   virt_addr_t addr = bytes_to_virt_addr(loc);
   fdt[fileID]->read_ptr = addr;                   
   printf("[DEBUG|ssfs_frseek] fdt[%d]: read at B: %d; O: %d\n", fileID, fdt[fileID]->read_ptr.d_ptr, fdt[fileID]->read_ptr.offset);

   return 0;
}

int ssfs_fwseek(int fileID, int loc){
   if(fdt[fileID] == NULL) {
      printf("[DEBUG|ssfs_fwseek] fileID %d is NULL\n", fileID);
      return -1;
   }
   if(fdt[fileID]->inode.size < loc+1 || loc < 0){// Bounds checking
      printf("[DEBUG|ssfs_fwseek] Out of bounds seek. loc: %d; filesize: %d\n", loc, fdt[fileID]->inode.size);
      return -1;
   }

   virt_addr_t addr = bytes_to_virt_addr(loc);
   fdt[fileID]->write_ptr = addr; 
   printf("[DEBUG|ssfs_frseek] fdt[%d]: write at B: %d; O: %d\n", fileID, fdt[fileID]->write_ptr.d_ptr, fdt[fileID]->write_ptr.offset);
   return 0;
}

int ssfs_fwrite(int fileID, char *buf, int length){
   if(fdt[fileID] == NULL) {
      printf("[DEBUG|ssfs_fwrite] fileID %d is NULL\n", fileID);
      return -1;
   }

   super_block_t *sb = malloc(BLOCK_SIZE);         // malloc                                    (10)
   read_blocks(SUPER_BLOCK, 1, sb);                // Retrieve super block: sb
   wm_t *WM = malloc(BLOCK_SIZE);                  // malloc                                    (11)
   read_blocks(WM_BLOCK, 1, WM);                   // Retrieve WM:          WM
   wm_t *FBM = malloc(BLOCK_SIZE);                 // malloc                                    (12)
   read_blocks(FBM_BLOCK, 1, FBM);                 // Retrieve FBM:         FBM
   int inode_id = fdt[fileID]->inode_id;           // Get inode ID

   while(length > 0) {                             // While there are bytes to write
      int *d_ptr_id = &fdt[fileID]->write_ptr.d_ptr;// Index of direct pointer
      int b_id = get_block_id(&fdt[fileID]->inode, *d_ptr_id);// Convert it to block pointer
      int *offset = &fdt[fileID]->write_ptr.offset;// Get offset

      if(b_id == 0) {                              // This means we need to write to a new block
         b_ptr_t new_block = get_unused_block();   // Get a free block to write the rest
         add_new_block(&fdt[fileID]->inode, inode_id, *d_ptr_id, new_block, sb);
      }

      if(WM->mask[b_id] == 0) {                    // If block is not writable
         // Do something
      }

      char *current_block = calloc(BLOCK_SIZE, 1); // Allocate a whole block                     (9)
      read_blocks(b_id, 1, current_block);         // Retrieve current_block

      // bytes to write = min(length, BLOCK_SIZE - offset of current write pointer)
      int bytes_to_write = length < BLOCK_SIZE-*offset ? length : BLOCK_SIZE-*offset;

      memcpy(&current_block[*offset], buf, bytes_to_write);// Write to block
      write_blocks(b_id, 1, current_block);        // Write block to disk

      // Need to update offset, block num, length, buf
      buf = buf + bytes_to_write;                  // Update buf
      length -= bytes_to_write;                    // Update length of buf
      *offset = (*offset + bytes_to_write) % BLOCK_SIZE; // Update offset (if whole block is written then offest will be reset to 0)          
      if(*offset == 0) {
         printf("[DEBUG|ssfs_fwrite] Incrementing direct pointer: %d\n", *d_ptr_id);
         (*d_ptr_id)++;                            // Update write d_ptr
      }
   }

   free(sb);                                       //                                           (10)
   free(WM);                                       //                                           (11)
   free(FBM);                                      //                                           (12)
   return 0;
}

int ssfs_fread(int fileID, char *buf, int length){
    return 0;
}
int ssfs_remove(char *file){
    return 0;
}

/**********************************************************************************************/

b_ptr_t get_block_id(inode_t *inode, int d_ptr_id) {
   if(d_ptr_id > (MAX_DIRECT_PTR + BLOCK_SIZE/sizeof(b_ptr_t))) {
      printf("[DEBUG|get_block_id] d_ptr_id is way too big: %d\n", d_ptr_id);
      printf("%d\n", MAX_DIRECT_PTR + (BLOCK_SIZE/sizeof(b_ptr_t)));
      return -1;
   }
   if(d_ptr_id > MAX_DIRECT_PTR) {              // Need to look into indirect ptr
      printf("[DEBUG|get_block_id] Looking into indirect pointer: %d\n", inode->i_ptr);
      b_ptr_t i_ptr = inode->i_ptr;             // Get indirect pointer

      ptr_file_t *ptr_file = malloc(BLOCK_SIZE);// Malloc                                    (13)
      read_blocks(i_ptr, 1, ptr_file);          // Retrieve pointer file

      b_ptr_t ptr = ptr_file->ptrs[d_ptr_id - MAX_DIRECT_PTR];
      free(ptr_file);                           // Free                                      (13)
      return ptr;
   }

   return inode->d_ptrs[d_ptr_id];
}

int add_new_block(inode_t *inode, int inode_id, int d_ptr_id, b_ptr_t new_block, super_block_t *sb) {
   if(d_ptr_id > MAX_DIRECT_PTR + BLOCK_SIZE/sizeof(b_ptr_t)) {
      printf("[DEBUG|add_new_block] d_ptr_id is way too big: %d\n", d_ptr_id);
      return -1;
   }
   if(sb == NULL) {
      printf("[DEBUG|add_new_block] sb is NULL\n");
      return -1;
   }

   if(d_ptr_id > MAX_DIRECT_PTR) {// Need to look into indirect ptr
      printf("[DEBUG|add_new_block] Looking into indirect pointer: %d\n", inode->i_ptr);
      b_ptr_t *i_ptr = &inode->i_ptr;           // Get indirect pointer

      if(*i_ptr == 0) {                         // If indirect pointer not yet initialized
         printf("[DEBUG|add_new_block] I-node %d full. Creating pointer file.\n", inode_id);
         *i_ptr = get_unused_block();           // "create" a new pointer file

         wm_t *FBM = malloc(BLOCK_SIZE);        // malloc                                 (16)
         read_blocks(FBM_BLOCK, 1, FBM);        // Retrieve FBM
         FBM->mask[*i_ptr] = 0;                 // Update new block status
         write_blocks(FBM_BLOCK, 1, FBM);       // Update FBM
         free(FBM);                             // Free                                   (16)
      }

      ptr_file_t *ptr_file = malloc(BLOCK_SIZE);// Malloc                                 (14)
      read_blocks(*i_ptr, 1, ptr_file);         // Retrieve pointer file

      ptr_file->ptrs[d_ptr_id - MAX_DIRECT_PTR] = new_block; // Update ptr
      write_blocks(*i_ptr, 1, ptr_file);        // Update pointer file
      free(ptr_file);                           // Free                                   (14)
      return 0;
   }

   // Getting the id of the inode table block that contains our inode.
   // Since int division truncates to 0, we do inode_id/number of inodes in a block
   b_ptr_t inode_block_id = get_block_id(&sb->current_root, inode_id/(BLOCK_SIZE/sizeof(inode_t)));

   inode_block_t *inode_block = malloc(BLOCK_SIZE);// Malloc                              (15)
   read_blocks(inode_block_id, 1, inode_block); // Retrieve inode block
   inode_block->inodes[inode_id % (BLOCK_SIZE/sizeof(inode_t))].d_ptrs[d_ptr_id] = new_block;
   write_blocks(inode_block_id, 1, inode_block);// Update inode block
   free(inode_block);                           // Free                                   (15)

   inode->d_ptrs[d_ptr_id] = new_block;         // Don't forget to update the in-mem inode
   return 0;
}

virt_addr_t bytes_to_virt_addr(int bytes) {     // Converts a byte number to a virtual address
   virt_addr_t thingy = { .d_ptr = bytes/BLOCK_SIZE, .offset = bytes%BLOCK_SIZE };
   return thingy;
}

int new_fdt_entry(inode_t inode, int inode_id) {// Creates a new entry in the FDT
   for(int i=0; i<NUM_BLOCKS; i++) {
      if(fdt[i] == NULL) {
         fd_t *new_entry = calloc(sizeof(fd_t), 1);

         new_entry->inode = inode;
         new_entry->inode_id = inode_id;        
//       new_entry->read_ptr = { .d_ptr = 0, offset = 0 };// Unnecessary because of calloc
         new_entry->write_ptr = bytes_to_virt_addr(inode.size);

         fdt[i] = new_entry;
         printf("[DEBUG|new_fdt_entry] Entry created: %d\n", i);
         return i;
      }
   }
   return -1;
}

b_ptr_t get_unused_block() {   // Gets an unused block (according to some strategy)
   wm_t *FBM = malloc(BLOCK_SIZE);
   read_blocks(FBM_BLOCK, 1, FBM);

   for(int i=0; i<BLOCK_SIZE; i++) {
      if(FBM->mask[i] = 1) {
         free(FBM);
         return i;
      }
   }
   free(FBM);
   return -1;
}
