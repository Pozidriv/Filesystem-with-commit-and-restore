#include "sfs_api.h"
#include "disk_emu.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define BLOCK_SIZE 1024             // Block size in bytes
#define NUM_BLOCKS 1024             // Number of block in the file system
#define MAGIC 0xACBD0005            // Magic number
#define NUM_SHADOW_ROOTS 14         // Maximum number of shadow roots

#define MAX_DIRECT_PTR 14           // Maximum number of direct pointers in an i/j-node stuct

#define SUPER_BLOCK 0               // Location of superblock on disk
#define DEFAULT_FBM_BLOCK 1         // Location of file bit mask on disk
#define DEFAULT_WM_BLOCK 2          // Location of write mask on disk
#define DEFAULT_INODE_TABLE_BLOCK 3 // Location of the very first inode table
#define DEFAULT_ROOT_DIR_BLOCK 4    // Location of root dir on disk init

#define DIR_ENTRY_SIZE 16           // Max size for a directory entry
#define FILENAME_SIZE 10            // Max size for the filename (includes extensions)

#define J_NODE 0              // j-node position in fdt
#define ROOT_DIR 1                 // root dir position in fdt

/**************************************************************************/

typedef int b_ptr_t;                // Pointer to a disk block

typedef struct _virt_addr_t {       // Address in inode
   int d_ptr;                       // Index of direct pointer number
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
   int current_root;                //
   inode_t roots[NUM_SHADOW_ROOTS]; // Array of shadow roots
   b_ptr_t wm_ptrs[NUM_SHADOW_ROOTS];
   b_ptr_t fbm_ptrs[NUM_SHADOW_ROOTS];
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
int add_new_block(inode_t*, int, int, b_ptr_t, super_block_t*, int); // Adds specified block to pointed sb
int new_fdt_entry(inode_t, int);    // Creates a new entry in the FDT
int get_free_inode();               // Gets a free inode (according to some strategy)
int get_inode_id(char*, super_block_t*);// Retrieves the ID of the inode of the file

int virt_addr_to_bytes(virt_addr_t);// Converts a virtual address it's bytes number
virt_addr_t bytes_to_virt_addr(int);// Converts a byte number to a virtual address
b_ptr_t get_block_id(inode_t*, int);// Safe conversion of pointer index to block pointer

/**************************************************************************/

fd_t *fdt[NUM_BLOCKS];              // File descriptor table

/**************************************************************************/

int ssfs_commit() {
   super_block_t *sb = calloc(BLOCK_SIZE, 1);
   read_blocks(SUPER_BLOCK, 1, sb);
   if(sb->current_root >= NUM_SHADOW_ROOTS) {
      printf("[DEBUG|ssfs_commit] Max number of shadow roots exceeded. Aborting\n");
      free(sb);
      return -1;
   }

   wm_t *WM = calloc(BLOCK_SIZE, 1);
   read_blocks(sb->wm_ptrs[sb->current_root], 1, WM);
   fbm_t *FBM = calloc(BLOCK_SIZE, 1);
   read_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM);
   
   b_ptr_t new_WM_block = get_unused_block();
   if(new_WM_block == -1) {
      printf("[DEBUG|ssfs_commit] Block allocation for new WM block failed. Aborting\n");
      return -1;
   }
   FBM->mask[new_WM_block] = 0;
   write_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM);

   b_ptr_t new_FBM_block = get_unused_block();
   if(new_FBM_block == -1) {
      printf("[DEBUG|ssfs_commit] Block allocation for new FBM block failed. Aborting\n");
      return -1;
   }
   FBM->mask[new_WM_block] = 0;
   write_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM);

   sb->wm_ptrs[sb->current_root+1] = new_WM_block;
   sb->fbm_ptrs[sb->current_root+1] = new_FBM_block;

   // Procede to mark all currently used blocks as read-only
   for(int i=0; i<BLOCK_SIZE; i++) {
      if(FBM->mask[i] == 0) WM->mask[i] = 0; 
   }

   sb->roots[sb->current_root+1] = sb->roots[sb->current_root]; // Copy current root
   sb->current_root++;              // Update current root number
   write_blocks(sb->wm_ptrs[sb->current_root], 1, WM);   // Write new WM
   write_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM); // Write new FBM
   write_blocks(SUPER_BLOCK, 1, sb);
   
   free(sb);
   free(WM);
   free(FBM);
   return sb->current_root-1;
}

int ssfs_restore(int cnum) {
   if(cnum < 0 || cnum > NUM_SHADOW_ROOTS) {
      printf("[DEBUG|ssfs_restore] cnum is out of bounds... wtf are you trying to do?\n");
      return -1;
   }

   super_block_t *sb = calloc(BLOCK_SIZE, 1);
   read_blocks(SUPER_BLOCK, 1, sb);
   sb->current_root = cnum;
   write_blocks(SUPER_BLOCK, 1, sb);
   free(sb);
   
   return 0;
}

void mkssfs(int fresh){
   if(fresh == 1) {              // Fresh disk -> need to perform first time setup
      if(init_fresh_disk("placeholder", BLOCK_SIZE, NUM_BLOCKS) == -1)
         exit(-1);

      // Creating superblock
      super_block_t *sb = calloc(BLOCK_SIZE, 1);   // Allocate a block to hold the superblock (1)

      sb->magic = MAGIC;
      sb->block_size = BLOCK_SIZE;
      sb->num_blocks = NUM_BLOCKS;
      sb->num_inodes = 1;                          // We start with 1 i-node for the root dir
      sb->roots[sb->current_root].size = sizeof(inode_t);   // Root is thus of size inode_t ??????

      // Creating root dir inode and placing it in first i-node block
      inode_block_t *ib = calloc(BLOCK_SIZE, 1);   // Allocate a block for the inodes         (4)
      ib->inodes[0].d_ptrs[0] = DEFAULT_ROOT_DIR_BLOCK; // inode 0 now points to root dir
//    ib->inodes[0].size = 0;                           // Not necessary (calloc)
      write_blocks(DEFAULT_INODE_TABLE_BLOCK, 1, ib);   // Write inode table
      sb->roots[sb->current_root].d_ptrs[0] = DEFAULT_INODE_TABLE_BLOCK; // Point to first inode table block

      // Write current root to fdt[0]. It's a special entry, so we don't care if id is 0
      new_fdt_entry(sb->roots[sb->current_root], -1);         // Add root in FDT (at index 0)

      new_fdt_entry(ib->inodes[0], 0);             // Add root dir in FDT (at index 1)
      free(ib);                                    // Free                                    (4)

      sb->wm_ptrs[sb->current_root] = DEFAULT_WM_BLOCK;
      sb->fbm_ptrs[sb->current_root] = DEFAULT_FBM_BLOCK;
      write_blocks(SUPER_BLOCK, 1, sb);            // Write the superblock
      free(sb);                                    // Free                                    (1)

      // Create FBM
      fbm_t *FBM = calloc(BLOCK_SIZE, 1);          // Allocate a whole block for the FBM      (2)

      memset(FBM->mask, 1, NUM_BLOCKS);            // Set the whole FBM to 1
      FBM->mask[SUPER_BLOCK] = 0;
      FBM->mask[DEFAULT_FBM_BLOCK] = 0;
      FBM->mask[DEFAULT_WM_BLOCK] = 0;             // This is not yet used, but will be shortly!
      FBM->mask[DEFAULT_INODE_TABLE_BLOCK] = 0;
      FBM->mask[DEFAULT_ROOT_DIR_BLOCK] = 0;

      write_blocks(DEFAULT_FBM_BLOCK, 1, FBM);     // Write the FBM
      free(FBM);                                   // Free                                    (2)

      // Create WM
      wm_t *WM = calloc(BLOCK_SIZE, 1);            // Allocate a whole block for the WM       (3)

      memset(WM->mask, 1, NUM_BLOCKS);             // Set the whole WM to 1
//    WM->mask[DEFAULT_ROOT_DIR_BLOCK] = 0;        // Set the root dir to be read-only

      write_blocks(DEFAULT_WM_BLOCK, 1, WM);               // Write the WM
      free(WM);                                    // Free                                    (3)
   } else {                      // Else assume it's already setup
      if(init_disk("placeholder", BLOCK_SIZE, NUM_BLOCKS) == -1)
         exit(-1);

      super_block_t *sb = calloc(BLOCK_SIZE, 1);
      read_blocks(SUPER_BLOCK, 1, sb);

      // Write current root to fdt[0]. It's a special entry, so we don't care if id is 0
      new_fdt_entry(sb->roots[sb->current_root], -1);         // Add root in FDT (at index 0)

      // Retrieve root dir inode
      inode_t *root_dir_inode = calloc(sizeof(inode_t), 1);
      ssfs_frseek(J_NODE, 0);
      ssfs_fread(J_NODE, (char*) root_dir_inode, sizeof(inode_t));

      new_fdt_entry(*root_dir_inode, 0);           // Add root dir in FDT (at index 1)
      free(root_dir_inode); 
      free(sb);
   }
}

int ssfs_fopen(char *name){
   printf("[DEBUG|ssfs_fopen] -------------------------\n");
   if(name == NULL) {
      printf("[DEBUG|ssfs_open] NULL filename. Aborting\n");
      return -1;
   }
   inode_t *inode = calloc(sizeof(inode_t), 1);    // Initialize an inode                     (7)
   super_block_t *sb = malloc(BLOCK_SIZE);         // malloc                                  (5)
   read_blocks(SUPER_BLOCK, 1, sb);                // Retrieve super block
   printf("[DEBUG|ssfs_fopen] Current root: %d, FBM block: %d, WM block: %d\n", sb->current_root, sb->fbm_ptrs[sb->current_root], sb->wm_ptrs[sb->current_root]);

   if(strlen(name) > FILENAME_SIZE) {
      printf("[DEBUG|ssfs_open] Inputted filename too big. Truncating\n");
      name[FILENAME_SIZE] = 0;
   }

   // Check if file exists
   int inode_id = get_inode_id(name, sb);

   if(inode_id == -1) {                            // File does not exist
      if(get_unused_block() == -1) {               // Check if there is still room
         printf("[DEBUG|ssfs_fopen] No more free blocks. Aborting\n");
         free(sb);                                 // Free                                    (5)
         free(inode);                              // Free                                    (7)
         return -1;
      }
      // Creating a dir entry and an inode. Not allocating any blocks yet
      inode_id = get_free_inode();
      printf("[DEBUG|ssfs_fopen] inode id is %d\n", inode_id);
      if(inode_id == -1) {                         // Means inode is appended at the end
         printf("[DEBUG|ssfs_fopen] Appending new inode at %d\n", sb->num_inodes);
         inode_id = sb->num_inodes;
      }
      sb->roots[sb->current_root].size += sizeof(inode_t);    //
      printf("[DEBUG|ssfs_fopen] New inode: %d\n", inode_id);
      if(ssfs_fwseek(J_NODE, inode_id*sizeof(inode_t)) < 0) return -1;
      if(ssfs_fwrite(J_NODE, (char*) inode, sizeof(inode_t)) <= 0) return -1;// Write inode to appropriate block
      sb->num_inodes++;                            // Update inode count

      dir_entry_t *entry = calloc(DIR_ENTRY_SIZE, 1);// Calloc                               (18)
      entry->inode_id = inode_id;
      strcpy(entry->filename, name);
      printf("[DEBUG|ssfs_fopen] Adding entry \"%s\" for inode %d at %d in dir\n", entry->filename, entry->inode_id, (inode_id-1)*DIR_ENTRY_SIZE);
      ssfs_fwseek(ROOT_DIR, (inode_id-1)*DIR_ENTRY_SIZE); // Seek to appropriate dir entry
      ssfs_fwrite(ROOT_DIR, (char*) entry, DIR_ENTRY_SIZE);
      free(entry);                                 // Free                                   (18)
   } else {                                        // File exists
      ssfs_frseek(J_NODE, inode_id*sizeof(inode_t));
      ssfs_fread(J_NODE, (char*) inode, sizeof(inode_t));
   }

   // Create FDT entry
   int fd = new_fdt_entry(*inode, inode_id);
   write_blocks(SUPER_BLOCK, 1, sb);
   free(sb);                                       // Free                                    (5)
   free(inode);                                    // Free                                    (7)

   if(fd != -1) printf("[DEBUG|ssfs_fopen] NEW ENTRY: fdt[%d] has inode %d\n", fd, fdt[fd]->inode_id);

   printf("[DEBUG|ssfs_fopen] Filename: \"%s\". Returning %d\n--------------------------------------\n\n", name, fd);

   return fd;
}

int ssfs_fclose(int fileID){
   printf("[DEBUG|ssfs_fclose] -------------------------\n");
   if(fileID == J_NODE || fileID == ROOT_DIR) {
      printf("[DEBUG|ssfs_fclose] Tried deleting entry %d. Aborting\n", fileID);
      return -1;
   }
   if(fileID < 0 || fileID > NUM_BLOCKS) {
      printf("[DEBUG|ssfs_fclose] Out of bounds index: %d. (don't be silly now)\n", fileID);
      return -1;
   }
   if(fdt[fileID] == NULL) {
      printf("[DEBUG|ssfs_fclose] FDT entry %d is NULL. Aborting (don't be silly now)\n", fileID);
      return -1;
   }
   printf("[DEBUG|ssfs_fclose] Closing %d\n", fileID);
   free(fdt[fileID]);                              // Free
   fdt[fileID] = NULL;                             // Reset pointer
// printf("[DEBUG|ssfs_fclose] Closed. Exiting\n");
   return 0;
}

int ssfs_frseek(int fileID, int loc){
   if(fileID < 0 || fileID > NUM_BLOCKS || fdt[fileID] == NULL) {
      printf("[DEBUG|ssfs_frseek] fileID %d is NULL\n", fileID);
      return -1;
   }
   if(loc < 0) { // I'll ask the prof about this
      return -1;
   }
   if(fdt[fileID]->inode.size < loc-1) {  // Bounds checking
      printf("[DEBUG|ssfs_frseek] Out of bounds seek. fileID: %d, loc: %d; filesize: %d\n", fileID, loc, fdt[fileID]->inode.size);
      fdt[fileID]->read_ptr = bytes_to_virt_addr(fdt[fileID]->inode.size);
      return -1;
   }

   virt_addr_t addr = bytes_to_virt_addr(loc);
   fdt[fileID]->read_ptr = addr;                   
   if(fileID != 0 && fileID != -1) printf("[DEBUG|ssfs_frseek] fdt[%d]: read at (%d,%d)\n", fileID, fdt[fileID]->read_ptr.d_ptr, fdt[fileID]->read_ptr.offset);

   return 0;
}

int ssfs_fwseek(int fileID, int loc){
   if(fileID < 0 || fileID > NUM_BLOCKS || fdt[fileID] == NULL) {
      printf("[DEBUG|ssfs_fwseek] fileID %d is NULL\n", fileID);
      return -1;
   }
   if(fdt[fileID]->inode.size < loc || loc < 0){// Bounds checking
      printf("[DEBUG|ssfs_fwseek] Out of bounds seek. fileID: %d, loc: %d; filesize: %d\n", fileID, loc, fdt[fileID]->inode.size);
      return -1;
   }

   virt_addr_t addr = bytes_to_virt_addr(loc);
   fdt[fileID]->write_ptr = addr; 
   if(fileID != 0 && fileID != -1) printf("[DEBUG|ssfs_fwseek] fdt[%d]: write at direct pointer: %d; offset: %d\n", fileID, fdt[fileID]->write_ptr.d_ptr, fdt[fileID]->write_ptr.offset);
   return 0;
}

int ssfs_fwrite(int fileID, char *buf, int length){
   if(fileID < 0 || fileID > NUM_BLOCKS || fdt[fileID] == NULL) {
      printf("[DEBUG|ssfs_fwrite] fileID %d is NULL\n", fileID);
      return -1;
   }
   int total_bytes_written = 0;

   super_block_t *sb = malloc(BLOCK_SIZE);         // malloc                                    (10)
   read_blocks(SUPER_BLOCK, 1, sb);                // Retrieve super block: sb
   wm_t *WM = malloc(BLOCK_SIZE);                  // malloc                                    (11)
   read_blocks(sb->wm_ptrs[sb->current_root], 1, WM);   // Retrieve WM:          WM
   wm_t *FBM = malloc(BLOCK_SIZE);                 // malloc                                    (12)
   read_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM); // Retrieve FBM:         FBM
   int inode_id = fdt[fileID]->inode_id;           // Get inode ID

   if(fileID != 0 && fileID != -1) printf("[DEBUG|ssfs_fwrite] fileID: %d, inode: %d, length: %d\n", fileID, fdt[fileID]->inode_id, length);

   while(length > 0) {                             // While there are bytes to write
      int *d_ptr_id = &fdt[fileID]->write_ptr.d_ptr;// Index of direct pointer
//    printf("[DEBUG|ssfs_fwrite] d_ptr_id is %d\n", *d_ptr_id);
      b_ptr_t b_id = get_block_id(&fdt[fileID]->inode, *d_ptr_id);// Convert it to block pointer
      int *offset = &fdt[fileID]->write_ptr.offset;// Get offset
      if(b_id == -1) {
         free(sb);
         free(WM);
         free(FBM);
         return -1;
      }
      if(fileID != 0 && fileID != -1) printf("[DEBUG|ssfs_fwrite] fileID: %d, inode: %d, length: %d, write at: (%d,%d)\n", fileID, fdt[fileID]->inode_id, length, *d_ptr_id, *offset);

      // bytes to write = min(length, BLOCK_SIZE - offset of current write pointer)
      int bytes_to_write = length < BLOCK_SIZE-*offset ? length : BLOCK_SIZE-*offset;

      if(b_id == 0) {                              // This means we need to write to a new block
         b_ptr_t new_block = get_unused_block();   // Get a free block to write the rest
         printf("[DEBUG|ssfs_fwrite] Writing to new block %d\n", new_block);
         if(new_block == -1) {
            printf("[DEBUG|ssfs_fwrite] No more free blocks. Aborting\n");
            free(sb);                              // Free                                      (10)
            free(WM);                              // Free                                      (11)
            free(FBM);                             // Free                                      (12)
            return -1;
         }
         add_new_block(&fdt[fileID]->inode, inode_id, *d_ptr_id, new_block, sb, bytes_to_write);
         b_id = new_block;
         printf("[DEBUG|ssfs_fwrite] Direct pointer %d maps to %d\n", *d_ptr_id, b_id);
         if(b_id == -1) {
            free(sb);                              // Free                                      (10)
            free(WM);                              // Free                                      (11)
            free(FBM);                             // Free                                      (12)
            return -1;
         }
      }

      if(WM->mask[b_id] == 0) {                    // If block is not writable
         b_ptr_t new_block = get_unused_block();
         printf("DEBUG|ssfs_fwrite] Block %d is marked as read-only! Using block %d instead\n", b_id, new_block);
    
         if(new_block == -1) {
            printf("[DEBUG|ssfs_fwrite] No more free blocks. Aborting\n");
            free(sb);                              // Free                                      (10)
            free(WM);                              // Free                                      (11)
            free(FBM);                             // Free                                      (12)
            return -1;
         }
         add_new_block(&fdt[fileID]->inode, inode_id, *d_ptr_id, new_block, sb, bytes_to_write);

         char *old_block = calloc(BLOCK_SIZE, 1);
         ssfs_frseek(fileID, (*d_ptr_id)*BLOCK_SIZE);
         ssfs_fread(fileID, old_block, BLOCK_SIZE);// Retrieve old block

         memcpy(&old_block[*offset], buf, bytes_to_write);// Copy on write
         write_blocks(new_block, 1, old_block);    // Write to new block
         free(old_block);
      } else {
         char *current_block = calloc(BLOCK_SIZE, 1); // Allocate a whole block                     (9)
         read_blocks(b_id, 1, current_block);         // Retrieve current_block

         printf("[DEBUG|ssfs_fwrite] Writing %d bytes into block %d at offset %d. Left to write: %d\n", bytes_to_write, b_id, *offset, length-bytes_to_write);
   //    printf("block start: %p, my offseted block: %p\n", current_block, &current_block[*offset]);

         memcpy(&current_block[*offset], buf, bytes_to_write);// Write to block
         char *str = calloc(bytes_to_write+1, 1);
         memcpy(str, buf, bytes_to_write);
         if(fileID == 17 || fileID == 67) printf("\nWriting:\n\n%s\n\n", str);
         free(str);
         write_blocks(b_id, 1, current_block);        // Write block to disk
         printf("[ssfs_fwrite] Wrote in block %d\n", b_id);
         free(current_block);                         // Free                                       (9)
      }

      // Need to update offset, block num, length, buf
      buf = buf + bytes_to_write;                  // Update buf
      length -= bytes_to_write;                    // Update length of buf
      total_bytes_written += bytes_to_write;

      // Increment size of file before moving wptr (maximum of filesize and write ptr+bytes written)
      fdt[fileID]->inode.size = fdt[fileID]->inode.size < virt_addr_to_bytes(fdt[fileID]->write_ptr) + bytes_to_write ? virt_addr_to_bytes(fdt[fileID]->write_ptr) + bytes_to_write : fdt[fileID]->inode.size;
      ssfs_fwseek(fileID, virt_addr_to_bytes(fdt[fileID]->write_ptr) + bytes_to_write);// move wptr
   }
   if(fileID != J_NODE) {               // If not the j-node
      printf("[DEBUG|ssfs_write] fdt[%d]: ID %d has size %d\n", fileID, fdt[fileID]->inode_id, fdt[fileID]->inode.size);
      ssfs_fwseek(J_NODE, fdt[fileID]->inode_id*sizeof(inode_t));
      ssfs_fwrite(J_NODE, (char*) &fdt[fileID]->inode, sizeof(inode_t)); // Update inode
//    printf("[DEBUG|ssfs_write] Bytes written: %d\n", total_bytes_written);
      printf("------------------------------------------------------\n");
      printf("[DEBUG|ssfs_fwrite] fdt[%d]: direct pointer 6 maps to %d\n", fileID, fdt[fileID]->inode.d_ptrs[6]);
   }

   free(sb);                                       // Free                                      (10)
   free(WM);                                       // Free                                      (11)
   free(FBM);                                      // Free                                      (12)
   return total_bytes_written;
}

int ssfs_fread(int fileID, char *buf, int length){
   printf("[DEBUG|sfs_fread] -------------------------\n");
   if(fileID < 0 || fileID > NUM_BLOCKS || fdt[fileID] == NULL) {
      printf("[DEBUG|ssfs_fread] fdt[%d] is NULL\n", fileID);
      return -1;
   }
   if(fileID != 0 && fileID != 1) printf("[DEBUG|ssfs_fread] Reading %d bytes of fdt[%d] at (%d,%d)\n", length, fileID, fdt[fileID]->read_ptr.d_ptr, fdt[fileID]->read_ptr.offset);

   int total_bytes_read = 0;
   if(fdt[fileID]->inode.size < virt_addr_to_bytes(fdt[fileID]->read_ptr) + length) {
      if(fileID != 0 && fileID != 1) printf("[DEBUG|ssfs_fread] Read too big. FS: %d, RP: (%d,%d), L: %d)\n", fdt[fileID]->inode.size, fdt[fileID]->read_ptr.d_ptr, fdt[fileID]->read_ptr.offset, length);
      length = fdt[fileID]->inode.size - virt_addr_to_bytes(fdt[fileID]->read_ptr);
   }
   while(length > 0) {
      int *d_ptr_id = &fdt[fileID]->read_ptr.d_ptr;// Index of direct pointer
      b_ptr_t b_id = get_block_id(&fdt[fileID]->inode, *d_ptr_id);// Convert it to block pointer
      int *offset = &fdt[fileID]->read_ptr.offset;// Get offset
      printf("[DEBUG|ssfs_fread] fdt[%d], inode: %d, Direct pointer %d maps to %d\n", fileID, fdt[fileID]->inode_id, *d_ptr_id, b_id);

      if(b_id == -1)
         return -1;

      char *current_block = calloc(BLOCK_SIZE, 1); // Allocate a whole block                    (17) 
      read_blocks(b_id, 1, current_block);         // Retrieve current_block

      // bytes to write = min(length, BLOCK_SIZE - offset of current write pointer)
      int bytes_to_read = length < BLOCK_SIZE-*offset ? length : BLOCK_SIZE-*offset;
      printf("[DEBUG|ssfs_fread] Reading %d bytes into block %d at offset %d. Left to read: %d\n", bytes_to_read, b_id, *offset, length-bytes_to_read);

      memcpy(buf, &current_block[*offset], bytes_to_read);
      length -= bytes_to_read;
      total_bytes_read += bytes_to_read;
      buf = &buf[bytes_to_read];

      ssfs_frseek(fileID, virt_addr_to_bytes(fdt[fileID]->read_ptr) + bytes_to_read);//move rptr
      free(current_block);                         // Free                                      (17)
   }
   printf("[DEBUG|ssfs_fread] End of read fileID: %d, inode: %d, size of read: %d, read pointer at (%d,%d)\n", fileID, fdt[fileID]->inode_id, total_bytes_read, fdt[fileID]->read_ptr.d_ptr, fdt[fileID]->read_ptr.offset);
   return total_bytes_read;
}

int ssfs_remove(char *file){
   printf("[DEBUG|ssfs_remove] -------------------------\n");
   super_block_t *sb = malloc(BLOCK_SIZE);
   read_blocks(SUPER_BLOCK, 1, sb);
   int inode_id = get_inode_id(file, sb);

   if(inode_id == -1) {
      printf("[DEBUG|ssfs_remove] File not found. Aborting\n");
      free(sb);
      return -1;
   }
   printf("[DEBUG|ssfs_remove] Removing inode %d\n", inode_id);

   for(int i=0; i<NUM_BLOCKS; i++) {            // Starting at 2 since 2 first entries are reserved
      if(fdt[i] == NULL || i == J_NODE || i == ROOT_DIR)
         continue;
      if(inode_id == fdt[i]->inode_id) {
//       printf("[DEBUG|ssfs_remove] Closing fdt[%d]\n", i);
         ssfs_fclose(i);
      }
   }
// printf("[DEBUG|ssfs_remove] FDT entries closed. Now removing inode\n");

   fbm_t *FBM = malloc(BLOCK_SIZE);
   wm_t *WM = malloc(BLOCK_SIZE);               // Necessary for read-only blocks
   read_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM);
   read_blocks(sb->wm_ptrs[sb->current_root], 1, WM);
   ssfs_frseek(J_NODE, inode_id*sizeof(inode_t));
   inode_t *inode = malloc(sizeof(inode_t));
   ssfs_fread(J_NODE, (char*) inode, sizeof(inode_t));  // Retrieve inode

   for(int i=0; i<(inode->size/BLOCK_SIZE); i++) {// Free all non read only blocks
      b_ptr_t block_to_free = get_block_id(inode, i);
      printf("[DEBUG|ssfs_remove] Freeing block %d\n", block_to_free);
      if(block_to_free == -1) break;
      if(WM->mask[block_to_free] == 1) FBM->mask[block_to_free] = 1;
   }
   if(inode->i_ptr != 0) {
      printf("[DEBUG|ssfs_remove] Freeing pointer block: %d\n", inode->i_ptr);
      FBM->mask[inode->i_ptr] = 1;
   }
   write_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM);
   free(FBM);
   free(WM);
   free(inode);

   inode_t *unused_inode = calloc(sizeof(inode_t), 1);
   unused_inode->size = -1;                     // Indicate inode is unused

   ssfs_fwseek(J_NODE, inode_id*sizeof(inode_t));    // Move write pointer of current root to inode
   ssfs_fwrite(J_NODE, (char*) unused_inode, sizeof(inode_t)); // Delete inode
   sb->num_inodes--;                            // Update number of inodes
   write_blocks(SUPER_BLOCK, 1, sb);

   printf("[DEBUG|ssfs_remove] Inodes left: %d. Now removing directory entry\n", sb->num_inodes);

   char *empty_array = calloc(DIR_ENTRY_SIZE, 1);
   ssfs_fwseek(ROOT_DIR, (inode_id-1)*DIR_ENTRY_SIZE);
   ssfs_fwrite(ROOT_DIR, empty_array, DIR_ENTRY_SIZE);

   free(empty_array);
   free(unused_inode);
   free(sb);

   return 0;
}

/**********************************************************************************************/

b_ptr_t get_block_id(inode_t *inode, int d_ptr_id) {
   printf("[DEBUG|get_block_id] -------------------------\n");
   if(d_ptr_id < 0) {
      printf("[DEBUG|get_block_id] d_ptr_id is negative: %d\n", d_ptr_id);
      return -1;
   }
   if(d_ptr_id >= (MAX_DIRECT_PTR + BLOCK_SIZE/sizeof(b_ptr_t))) {
      printf("[DEBUG|get_block_id] d_ptr_id is way too big: %d\n", d_ptr_id);
      return -1;
   }
   if(d_ptr_id >= MAX_DIRECT_PTR) {              // Need to look into indirect ptr
      printf("[DEBUG|get_block_id] Looking into indirect pointer: %d\n", inode->i_ptr);
      b_ptr_t i_ptr = inode->i_ptr;             // Get indirect pointer
      if(i_ptr == 0) return 0;

      ptr_file_t *ptr_file = malloc(BLOCK_SIZE);// Malloc                                    (13)
      read_blocks(i_ptr, 1, ptr_file);          // Retrieve pointer file

      b_ptr_t ptr = ptr_file->ptrs[d_ptr_id - MAX_DIRECT_PTR];
      free(ptr_file);                           // Free                                      (13)
      if(ptr>NUM_BLOCKS-1) {
         printf("[DEBUG|get_block_id] Block number out of bounds\n");
         return -1;
      }
      return ptr;
   }
   if(inode->d_ptrs[d_ptr_id]>NUM_BLOCKS) {
      printf("[DEBUG|get_block_id] Block number out of bounds\n");
      return -1;
   }

   return inode->d_ptrs[d_ptr_id];
}

int add_new_block(inode_t *inode, int inode_id, int d_ptr_id, b_ptr_t new_block, super_block_t *sb, int write_size) {
   printf("[DEBUG|add_new_block] -------------------------\n");
   if(d_ptr_id >= MAX_DIRECT_PTR + BLOCK_SIZE/sizeof(b_ptr_t)) {
      printf("[DEBUG|add_new_block] d_ptr_id is way too big: %d\n", d_ptr_id);
      return -1;
   }
   if(d_ptr_id < 0) {
      printf("[DEBUG|add_new_block] d_ptr_id is negative\n");
      return -1;
   }
   if(sb == NULL) {
      printf("[DEBUG|add_new_block] sb is NULL\n");
      return -1;
   }
   char *empty_block = calloc(BLOCK_SIZE, 1);
   write_blocks(new_block, 1, empty_block);     // Wipe out block
   printf("[add_new_block] Wrote in block %d --1--\n", new_block);
   free(empty_block);

   if(d_ptr_id >= MAX_DIRECT_PTR) {// Need to look into indirect ptr
      printf("[DEBUG|add_new_block] Looking into indirect pointer: %d\n", inode->i_ptr);
      b_ptr_t *i_ptr = &inode->i_ptr;           // Get indirect pointer

      wm_t *FBM = malloc(BLOCK_SIZE);           // malloc                                 (16)
      read_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM);           // Retrieve FBM
      FBM->mask[new_block] = 0;                 // Update new block
      write_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM);          // Update FBM

      if(*i_ptr == 0 || d_ptr_id == 14) {       // If indirect pointer not yet initialized
         *i_ptr = get_unused_block();           // "create" a new pointer file
         printf("[DEBUG|add_new_block] I-node %d full. Creating pointer file at %d\n", inode_id, *i_ptr);
         if(*i_ptr == -1)
            return -1;

         FBM->mask[*i_ptr] = 0;                 // Update pointer block status
         write_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM);       // Update FBM
         inode->i_ptr = *i_ptr;
         if(inode_id == -1) {                   // j-node: special procedure
      //    printf("[DEBUG|add_new_block] Adding new block %d at %d in j-node\n", new_block, d_ptr_id);
            super_block_t *sb = calloc(BLOCK_SIZE, 1);
            read_blocks(SUPER_BLOCK, 1, sb);
            
            sb->roots[sb->current_root].i_ptr = *i_ptr;
            write_blocks(SUPER_BLOCK, 1, sb);
            free(sb);
         } else {                               // normal i-node procedure
            b_ptr_t inode_block_id = get_block_id(&sb->roots[sb->current_root],inode_id/(BLOCK_SIZE/sizeof(inode_t)));

            if(inode_block_id == -1)
               return -1;
            inode_block_t *inode_block = malloc(BLOCK_SIZE);// Malloc                              (15)
            read_blocks(inode_block_id, 1, inode_block); // Retrieve inode block
            inode_block->inodes[inode_id % (BLOCK_SIZE/sizeof(inode_t))].i_ptr = inode->i_ptr;
            inode_block->inodes[inode_id % (BLOCK_SIZE/sizeof(inode_t))].size += write_size;
            write_blocks(inode_block_id, 1, inode_block);// Update inode block
            printf("[add_new_block] Wrote in block %d. Is indirect ptr for inode %d --2--\n", inode_block_id, inode_id);
            free(inode_block);                           // Free                                   (15)
         }
      }

      ptr_file_t *ptr_file = malloc(BLOCK_SIZE);// Malloc                                 (14)
      read_blocks(*i_ptr, 1, ptr_file);         // Retrieve pointer file

      ptr_file->ptrs[d_ptr_id - MAX_DIRECT_PTR] = new_block; // Update ptr
      write_blocks(*i_ptr, 1, ptr_file);        // Update pointer file
      printf("[add_new_block] Wrote in block %d. Is indirect ptr for inode %d --3--\n", *i_ptr, inode_id);
      free(ptr_file);                           // Free                                   (14)
      free(FBM);                                // Free                                   (16)


      return 0;
   }

   if(inode_id == -1) {                         // j-node: special procedure
//    printf("[DEBUG|add_new_block] Adding new block %d at %d in j-node\n", new_block, d_ptr_id);
      super_block_t *sb = calloc(BLOCK_SIZE, 1);
      read_blocks(SUPER_BLOCK, 1, sb);
      
      sb->roots[sb->current_root].d_ptrs[d_ptr_id] = new_block;
      write_blocks(SUPER_BLOCK, 1, sb);
      fbm_t *FBM = malloc(BLOCK_SIZE);
      read_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM);
      FBM->mask[new_block] = 0;
      write_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM);             // Update FBM
      free(FBM);
      free(sb);
      inode->d_ptrs[d_ptr_id] = new_block;         // Don't forget to update the in-mem inode
   } else {                                     // normal i-node procedure
      // Getting the id of the inode table block that contains our inode.
      // Since int division truncates to 0, we do inode_id/number of inodes in a block
      b_ptr_t inode_block_id = get_block_id(&sb->roots[sb->current_root],inode_id/(BLOCK_SIZE/sizeof(inode_t)));

      if(inode_block_id == -1)
         return -1;
      inode->d_ptrs[d_ptr_id] = new_block;         // Don't forget to update the in-mem inode
      inode_t *inode_to_write_back = calloc(sizeof(inode_t), 1);
      *inode_to_write_back = *inode;
      inode_to_write_back->size += write_size;

      ssfs_fwseek(J_NODE, inode_id*sizeof(inode_t));
      ssfs_fwrite(J_NODE, (char*) inode_to_write_back, sizeof(inode_t));
/*
      inode_block_t *inode_block = malloc(BLOCK_SIZE);// Malloc                              (15)
      read_blocks(inode_block_id, 1, inode_block); // Retrieve inode block
      inode_block->inodes[inode_id % (BLOCK_SIZE/sizeof(inode_t))].d_ptrs[d_ptr_id] = new_block;
      inode_block->inodes[inode_id % (BLOCK_SIZE/sizeof(inode_t))].size += write_size;
      write_blocks(inode_block_id, 1, inode_block);// Update inode block
      printf("[add_new_block] Wrote in block %d --4--\n", inode_block_id);
      free(inode_block);                           // Free                                   (15)
*/
   }
// inode->size += write_size;                   // Done in caller

   fbm_t *FBM = malloc(BLOCK_SIZE);
   read_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM);
   FBM->mask[new_block] = 0;
   write_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM);             // Update FBM
   free(FBM);

   return 0;
}

virt_addr_t bytes_to_virt_addr(int bytes) {     // Converts a byte number to a virtual address
   virt_addr_t thingy = { .d_ptr = bytes/BLOCK_SIZE, .offset = bytes%BLOCK_SIZE };
   return thingy;
}

int virt_addr_to_bytes(virt_addr_t addr) {      // ASSUMING OFFSET IS IN BYTES
   return addr.d_ptr*BLOCK_SIZE + addr.offset;
}

int new_fdt_entry(inode_t inode, int inode_id) {// Creates a new entry in the FDT
   printf("[DEBUG|new_fdt_entry] -------------------------\n");
   for(int i=0; i<NUM_BLOCKS-65; i++) {
      if(fdt[i] == NULL) {
         fd_t *new_entry = calloc(sizeof(fd_t), 1);

         new_entry->inode = inode;
         new_entry->inode_id = inode_id;        
//       new_entry->read_ptr = { .d_ptr = 0, offset = 0 };// Unnecessary because of calloc
         new_entry->write_ptr = bytes_to_virt_addr(inode.size);

         fdt[i] = new_entry;
//       printf("[DEBUG|new_fdt_entry] Entry created: %d now has inode %d\n", i, inode_id);
         return i;
      }
   }
   return -1;
}

b_ptr_t get_unused_block() {   // Gets an unused block (according to some strategy)
   printf("[DEBUG|get_unused_block] -------------------------\n");
   super_block_t *sb = calloc(BLOCK_SIZE, 1);
   read_blocks(SUPER_BLOCK, 1, sb);
   wm_t *FBM = malloc(BLOCK_SIZE);
   read_blocks(sb->fbm_ptrs[sb->current_root], 1, FBM);

   for(int i=0; i<BLOCK_SIZE; i++) {
      if(FBM->mask[i] == 1) {
         free(FBM);
         return i;
      }
   }
   free(FBM);
   free(sb);
// printf("[DEBUG|get_unused_block] !!! WARNING !!! No more free blocks\n");
   return -1;
}

int get_free_inode() {          // Gets a free inode and returns its ID
   printf("[DEBUG|get_free_inode] -------------------------\n");
   // Look into directory for the first gap. Pick that gap.
   inode_block_t *inode_block = calloc(BLOCK_SIZE, 1);
   ssfs_frseek(J_NODE, 0);                       // Read from beginning of j-node
   int d_ptr = 0;
   while(ssfs_fread(J_NODE, (char*) inode_block, BLOCK_SIZE) > 0) {
      for(int i=0; i<BLOCK_SIZE/sizeof(inode_t); i++) {
         if(inode_block->inodes[i].size == -1) {
            free(inode_block);
//          printf("[DEBUG|get_free_inode] Inode found: %d\n", d_ptr*BLOCK_SIZE/sizeof(inode_t)+i);
            return d_ptr*BLOCK_SIZE/sizeof(inode_t) + i; // Return ID
         }
      }
      d_ptr++;
   }
   free(inode_block);
   return -1;
}

int get_inode_id(char *name, super_block_t *sb) {
   printf("[DEBUG|get_inode_id] -------------------------\n");
   int inode_id = -1;                              // ID of the inode in the inode list 
   virt_addr_t addr = { .d_ptr = 0, .offset = 0 }; // Offset is in bytes
   int num_entries = BLOCK_SIZE/DIR_ENTRY_SIZE;
   dir_t *dir_block = calloc(BLOCK_SIZE, 1);       // calloc                                  (6)

// printf("[DEBUG|get_inode_id] Searching until direct pointer %d\n", sb->num_inodes/num_entries);
   while(addr.d_ptr*num_entries + addr.offset < fdt[ROOT_DIR]->inode.size) {
//    printf("[DEBUG|get_inode_id] Search at (%d,%d)\n", addr.d_ptr, addr.offset);
      ssfs_frseek(ROOT_DIR, virt_addr_to_bytes(addr));

      if(!(ssfs_fread(ROOT_DIR, (char*) dir_block, BLOCK_SIZE) > 0)) { // If read fails -> end of dir file
//       printf("[DEBUG|get_inode_id] File not found. At (%d,%d)\n", addr.d_ptr, addr.offset);
         break;
      }
      while(addr.offset<BLOCK_SIZE && addr.d_ptr*num_entries+addr.offset<fdt[ROOT_DIR]->inode.size) {
//       printf("[DEBUG|get_inode_id] Search at (%d,%d)\n",addr.d_ptr, addr.offset);
         if(strncmp(name, dir_block->files[addr.offset/DIR_ENTRY_SIZE].filename, FILENAME_SIZE) == 0) { // Compare filename
//          printf("[DEBUG|get_inode_id] File found. At (%d,%d)\n", addr.d_ptr, addr.offset);
            inode_id = dir_block->files[addr.offset/DIR_ENTRY_SIZE].inode_id;
//          printf("[DEBUG|get_inode_id] Currently looking at: \"%s\", %d\n", dir_block->files[addr.offset/DIR_ENTRY_SIZE].filename, dir_block->files[addr.offset/DIR_ENTRY_SIZE].inode_id);
            break;
         }
//       printf("[DEBUG|get_inode_id] Currently looking at: \"%s\", %d\n", dir_block->files[addr.offset/DIR_ENTRY_SIZE].filename, dir_block->files[addr.offset/DIR_ENTRY_SIZE].inode_id);
         addr.offset += DIR_ENTRY_SIZE;
      }
      if(inode_id != -1)
         break;
      addr.d_ptr++;                                // Increment block count
      addr.offset = 0;                             // Reset offset
   }
   free(dir_block);                                // Free                                     (6)
   return inode_id;
}
