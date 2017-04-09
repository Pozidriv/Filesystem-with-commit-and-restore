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
      sb->current_root.size = sizeof(inode_t);     // Root is thus of size inode_t ??????

      // Creating root dir inode and placing it in first i-node block
      inode_block_t *ib = calloc(BLOCK_SIZE, 1);   // Allocate a block for the inodes         (4)
      ib->inodes[0].d_ptrs[0] = DEFAULT_ROOT_DIR_BLOCK; // inode 0 now points to root dir
//    ib->inodes[0].size = 0;                           // Not necessary (calloc)
      write_blocks(DEFAULT_INODE_TABLE_BLOCK, 1, ib);   // Write inode table

      sb->current_root.d_ptrs[0] = DEFAULT_INODE_TABLE_BLOCK; // Point to first inode table block

      // Write current root to fdt[0]. It's a special entry, so we don't care if id is 0
      new_fdt_entry(sb->current_root, -1);         // Add root in FDT (at index 0)

      new_fdt_entry(ib->inodes[0], 0);             // Add root dir in FDT (at index 1)
      free(ib);                                    // Free                                    (4)

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

      super_block_t *sb = calloc(BLOCK_SIZE, 1);                                             //2
      read_blocks(SUPER_BLOCK, 1, sb);

      // Write current root to fdt[0]. It's a special entry, so we don't care if id is 0
      new_fdt_entry(sb->current_root, -1);         // Add root in FDT (at index 0)

      // Retrieve root dir inode
      inode_t *root_dir_inode = calloc(sizeof(inode_t), 1);                                  //1
      ssfs_frseek(J_NODE, 0);
      ssfs_fread(J_NODE, (char*) root_dir_inode, sizeof(inode_t));

      new_fdt_entry(*root_dir_inode, 0);           // Add root dir in FDT (at index 1)
      free(root_dir_inode);                                                                  //1
      free(sb);                                                                              //2
   }
}

int ssfs_fopen(char *name){
   if(name == NULL) return -1;

   inode_t *inode = calloc(sizeof(inode_t), 1);    // Initialize an inode                     (7)
   super_block_t *sb = malloc(BLOCK_SIZE);         // malloc                                  (5)
   read_blocks(SUPER_BLOCK, 1, sb);                // Retrieve super block

   int inode_id = get_inode_id(name, sb);          // Check if file exists

   if(inode_id == -1) {                            // If file does not exist
      if(get_unused_block() == -1) {               // Check if there is still room
         printf("[DEBUG|ssfs_fopen] No more free blocks. Aborting\n");
         free(sb);                                 // Free                                    (5)
         free(inode);                              // Free                                    (7)
         return -1;
      }
      
      inode_id = get_free_inode();                 // Creating a dir entry and an inode. Not allocating any blocks yet
      if(inode_id == -1)                           // Means inode is appended at the end
         inode_id = sb->num_inodes;

      sb->current_root.size += sizeof(inode_t);    //
      if(ssfs_fwseek(J_NODE, inode_id*sizeof(inode_t)) < 0) return -1;
      if(ssfs_fwrite(J_NODE, (char*) inode, sizeof(inode_t)) <= 0) return -1;// Write inode to appropriate block
      sb->num_inodes++;                            // Update inode count

      dir_entry_t *entry = calloc(DIR_ENTRY_SIZE, 1);// Calloc                               (18)
      entry->inode_id = inode_id;
      strcpy(entry->filename, name);
      ssfs_fwseek(ROOT_DIR, (inode_id-1)*DIR_ENTRY_SIZE); // Seek to appropriate dir entry
      ssfs_fwrite(ROOT_DIR, (char*) entry, DIR_ENTRY_SIZE);
      free(entry);                                 // Free                                   (18)
   } else {                                        // If file exists
      ssfs_frseek(J_NODE, inode_id*sizeof(inode_t));
      ssfs_fread(J_NODE, (char*) inode, sizeof(inode_t));
   }
   int fd = new_fdt_entry(*inode, inode_id);       // Create FDT entry
   write_blocks(SUPER_BLOCK, 1, sb);
   free(sb);                                       // Free                                    (5)
   free(inode);                                    // Free                                    (7)

   return fd;
}

int ssfs_fclose(int fileID){
   if(fileID == J_NODE || fileID == ROOT_DIR || fileID < 0 || fileID > NUM_BLOCKS || fdt[fileID] == NULL )// Bounds checking
      return -1;
   free(fdt[fileID]);                              // Free
   fdt[fileID] = NULL;                             // Reset pointer
   return 0;
}

int ssfs_frseek(int fileID, int loc){
   if(fileID < 0 || fileID > NUM_BLOCKS || fdt[fileID] == NULL || loc < 0) // Bounds checking
      return -1;
   if(fdt[fileID]->inode.size < loc-1) {           // If seek is too big, seek up to end of file but return -1
      fdt[fileID]->read_ptr = bytes_to_virt_addr(fdt[fileID]->inode.size);
      return -1;
   }
   virt_addr_t addr = bytes_to_virt_addr(loc);
   fdt[fileID]->read_ptr = addr;                   
   return 0;
}

int ssfs_fwseek(int fileID, int loc){
   if(fileID < 0 || fileID > NUM_BLOCKS || fdt[fileID] == NULL || fdt[fileID]->inode.size < loc || loc < 0)// Bounds checking
      return -1;
   virt_addr_t addr = bytes_to_virt_addr(loc);
   fdt[fileID]->write_ptr = addr; 
   return 0;
}

int ssfs_fwrite(int fileID, char *buf, int length){
   if(fileID < 0 || fileID > NUM_BLOCKS || fdt[fileID] == NULL) return -1;
   int total_bytes_written = 0;

   super_block_t *sb = malloc(BLOCK_SIZE);         // malloc                                    (10)
   read_blocks(SUPER_BLOCK, 1, sb);                // Retrieve super block: sb
   wm_t *WM = malloc(BLOCK_SIZE);                  // malloc                                    (11)
   read_blocks(WM_BLOCK, 1, WM);                   // Retrieve WM:          WM
   wm_t *FBM = malloc(BLOCK_SIZE);                 // malloc                                    (12)
   read_blocks(FBM_BLOCK, 1, FBM);                 // Retrieve FBM:         FBM
   int inode_id = fdt[fileID]->inode_id;           // Get inode ID

   while(length > 0) {                             // While there are bytes to write
      int *d_ptr_id = &fdt[fileID]->write_ptr.d_ptr;// Index of direct pointer
      b_ptr_t b_id = get_block_id(&fdt[fileID]->inode, *d_ptr_id);// Convert it to block pointer
      int *offset = &fdt[fileID]->write_ptr.offset;// Get offset
      if(b_id == -1) {
         free(sb);                                 // Free                                      (10)
         free(WM);                                 // Free                                      (11)
         free(FBM);                                // Free                                      (12)
         return -1;
      }
      // bytes to write = min(length, BLOCK_SIZE - offset of current write pointer)
      int bytes_to_write = length < BLOCK_SIZE-*offset ? length : BLOCK_SIZE-*offset;

      if(b_id == 0) {                              // This means we need to write to a new block
         b_ptr_t new_block = get_unused_block();   // Get a free block to write the rest
         if(new_block == -1) {
            free(sb);                              // Free                                      (10)
            free(WM);                              // Free                                      (11)
            free(FBM);                             // Free                                      (12)
            return -1;
         }
         add_new_block(&fdt[fileID]->inode, inode_id, *d_ptr_id, new_block, sb, bytes_to_write);
         b_id = new_block;
         if(b_id == -1) {
            free(sb);                              // Free                                      (10)
            free(WM);                              // Free                                      (11)
            free(FBM);                             // Free                                      (12)
            return -1;
         }
      }
      if(WM->mask[b_id] == 0) {                    // If block is not writable
         // Do something
      }
      char *current_block = calloc(BLOCK_SIZE, 1); // Allocate a whole block                     (9)
      read_blocks(b_id, 1, current_block);         // Retrieve current_block

      memcpy(&current_block[*offset], buf, bytes_to_write);// Write to block
      write_blocks(b_id, 1, current_block);        // Write block to disk
      free(current_block);                         // Free                                       (9)

      // Need to update offset, block num, length, buf
      buf = buf + bytes_to_write;                  // Update buf
      length -= bytes_to_write;                    // Update length of buf
      total_bytes_written += bytes_to_write;

      // Increment size of file before moving wptr (maximum of filesize and write ptr+bytes written)
      fdt[fileID]->inode.size = fdt[fileID]->inode.size < virt_addr_to_bytes(fdt[fileID]->write_ptr) + bytes_to_write ? virt_addr_to_bytes(fdt[fileID]->write_ptr) + bytes_to_write : fdt[fileID]->inode.size;
      ssfs_fwseek(fileID, virt_addr_to_bytes(fdt[fileID]->write_ptr) + bytes_to_write);// move wptr
   }
   if(fileID != J_NODE) {                          // If not the j-node
      ssfs_fwseek(J_NODE, fdt[fileID]->inode_id*sizeof(inode_t));
      ssfs_fwrite(J_NODE, (char*) &fdt[fileID]->inode, sizeof(inode_t)); // Update inode
   }

   free(sb);                                       // Free                                      (10)
   free(WM);                                       // Free                                      (11)
   free(FBM);                                      // Free                                      (12)
   return total_bytes_written;
}

int ssfs_fread(int fileID, char *buf, int length){
   if(fileID < 0 || fileID > NUM_BLOCKS || fdt[fileID] == NULL)
      return -1;

   int total_bytes_read = 0;
   if(fdt[fileID]->inode.size < virt_addr_to_bytes(fdt[fileID]->read_ptr) + length) // Truncate length if length too big
      length = fdt[fileID]->inode.size - virt_addr_to_bytes(fdt[fileID]->read_ptr);
   while(length > 0) {
      int *d_ptr_id = &fdt[fileID]->read_ptr.d_ptr;// Index of direct pointer
      b_ptr_t b_id = get_block_id(&fdt[fileID]->inode, *d_ptr_id);// Convert it to block pointer
      int *offset = &fdt[fileID]->read_ptr.offset;// Get offset

      if(b_id == -1) return -1;

      char *current_block = calloc(BLOCK_SIZE, 1); // Allocate a whole block                    (17) 
      read_blocks(b_id, 1, current_block);         // Retrieve current_block

      // bytes to write = min(length, BLOCK_SIZE - offset of current write pointer)
      int bytes_to_read = length < BLOCK_SIZE-*offset ? length : BLOCK_SIZE-*offset;
      memcpy(buf, &current_block[*offset], bytes_to_read);// Perform read

      length -= bytes_to_read;                     // Update length
      total_bytes_read += bytes_to_read;           // Increment total bytes read
      buf = &buf[bytes_to_read];                   // Increment buf pointer

      ssfs_frseek(fileID, virt_addr_to_bytes(fdt[fileID]->read_ptr) + bytes_to_read);//move rptr
      free(current_block);                         // Free                                      (17)
   }
   return total_bytes_read;
}

int ssfs_remove(char *file){
   super_block_t *sb = malloc(BLOCK_SIZE);         // malloc                                    //3
   read_blocks(SUPER_BLOCK, 1, sb);
   int inode_id = get_inode_id(file, sb);

   if(inode_id == -1) {
      printf("[DEBUG|ssfs_remove] File not found. Aborting\n");
      free(sb);                                                                                 //3
      return -1;
   }

   for(int i=0; i<NUM_BLOCKS; i++) {               // Starting at 2 since 2 first entries are reserved
      if(fdt[i] == NULL || i == J_NODE || i == ROOT_DIR) continue; // Ignore if null
      if(inode_id == fdt[i]->inode_id) ssfs_fclose(i);   // Close if is an entry for our file
   }

   fbm_t *FBM = malloc(BLOCK_SIZE);                                                             //4
   wm_t *WM = malloc(BLOCK_SIZE);                  // Necessary for read-only blocks            //5
   read_blocks(FBM_BLOCK, 1, FBM);
   read_blocks(WM_BLOCK, 1, WM);
   ssfs_frseek(J_NODE, inode_id*sizeof(inode_t));
   inode_t *inode = malloc(sizeof(inode_t));                                                    //6
   ssfs_fread(J_NODE, (char*) inode, sizeof(inode_t));  // Retrieve inode

   // Time to free everything we gave to the inode
   for(int i=0; i<(inode->size/BLOCK_SIZE); i++) {
      b_ptr_t block_to_free = get_block_id(inode, i);
      if(block_to_free == -1) break;
      if(WM->mask[block_to_free] == 1) FBM->mask[block_to_free] = 1;
   }
   if(inode->i_ptr != 0) FBM->mask[inode->i_ptr] = 1;
   write_blocks(FBM_BLOCK, 1, FBM);
   free(FBM);                                                                                   //4
   free(WM);                                                                                    //5
   free(inode);                                                                                 //6

   inode_t *unused_inode = calloc(sizeof(inode_t), 1);                                          //7
   unused_inode->size = -1;                        // Indicate inode is unused

   ssfs_fwseek(J_NODE, inode_id*sizeof(inode_t));  // Move write pointer of current root to inode
   ssfs_fwrite(J_NODE, (char*) unused_inode, sizeof(inode_t)); // Delete inode
   sb->num_inodes--;                               // Update number of inodes
   write_blocks(SUPER_BLOCK, 1, sb);

   // Removing directory entry
   char *empty_array = calloc(DIR_ENTRY_SIZE, 1);                                               //8
   ssfs_fwseek(ROOT_DIR, (inode_id-1)*DIR_ENTRY_SIZE);
   ssfs_fwrite(ROOT_DIR, empty_array, DIR_ENTRY_SIZE);

   free(empty_array);                                                                           //8
   free(unused_inode);                                                                          //7
   free(sb);                                                                                    //3
   return 0;
}

/**********************************************************************************************/

b_ptr_t get_block_id(inode_t *inode, int d_ptr_id) {
   if(d_ptr_id < 0 || d_ptr_id >= (MAX_DIRECT_PTR + BLOCK_SIZE/sizeof(b_ptr_t)))
      return -1;

   if(d_ptr_id >= MAX_DIRECT_PTR) {             // Need to look into indirect ptr
      b_ptr_t i_ptr = inode->i_ptr;             // Get indirect pointer
      if(i_ptr == 0) return 0;                  // If indirect pointer not initialized, delegate to caller

      ptr_file_t *ptr_file = malloc(BLOCK_SIZE);// Malloc                                    (13)
      read_blocks(i_ptr, 1, ptr_file);          // Retrieve pointer file

      b_ptr_t ptr = ptr_file->ptrs[d_ptr_id - MAX_DIRECT_PTR];
      free(ptr_file);                           // Free                                      (13)
      if(ptr > NUM_BLOCKS-1) return -1;
      return ptr;
   }
   if(inode->d_ptrs[d_ptr_id]>NUM_BLOCKS)
      return -1;

   return inode->d_ptrs[d_ptr_id];
}

int add_new_block(inode_t *inode, int inode_id, int d_ptr_id, b_ptr_t new_block, super_block_t *sb, int write_size) {
   if(d_ptr_id >= MAX_DIRECT_PTR + BLOCK_SIZE/sizeof(b_ptr_t) || sb == NULL || d_ptr_id < 0)
      return -1;

   char *empty_block = calloc(BLOCK_SIZE, 1);                                             //9
   write_blocks(new_block, 1, empty_block);     // Wipe out block
   free(empty_block);                                                                     //9

   if(d_ptr_id >= MAX_DIRECT_PTR) {// Need to look into indirect ptr
      b_ptr_t *i_ptr = &inode->i_ptr;           // Get indirect pointer

      wm_t *FBM = malloc(BLOCK_SIZE);           // malloc                                 (16)
      read_blocks(FBM_BLOCK, 1, FBM);           // Retrieve FBM
      FBM->mask[new_block] = 0;                 // Update new block
      write_blocks(FBM_BLOCK, 1, FBM);          // Update FBM

      if(*i_ptr == 0 || d_ptr_id == 14) {                         // If indirect pointer not yet initialized
         *i_ptr = get_unused_block();           // "create" a new pointer file
         if(*i_ptr == -1) {
            free(FBM);                          // Free                                   (16)
            return -1;
         }

         FBM->mask[*i_ptr] = 0;                 // Update pointer block status
         write_blocks(FBM_BLOCK, 1, FBM);       // Update FBM
         inode->i_ptr = *i_ptr;
         if(inode_id == -1) {                   // j-node: special procedure
            super_block_t *sb = calloc(BLOCK_SIZE, 1);                                    //10
            read_blocks(SUPER_BLOCK, 1, sb);
            sb->current_root.i_ptr = *i_ptr;
            write_blocks(SUPER_BLOCK, 1, sb);
            free(sb);                                                                     //10
         } else {                               // normal i-node procedure
            b_ptr_t inode_block_id = get_block_id(&sb->current_root,inode_id/(BLOCK_SIZE/sizeof(inode_t)));

            if(inode_block_id == -1) {
               free(FBM);                       // Free                                   (16)
               return -1;
            }
            inode_block_t *inode_block = malloc(BLOCK_SIZE);// Malloc                              (15)
            read_blocks(inode_block_id, 1, inode_block); // Retrieve inode block
            inode_block->inodes[inode_id % (BLOCK_SIZE/sizeof(inode_t))].i_ptr = inode->i_ptr;
            inode_block->inodes[inode_id % (BLOCK_SIZE/sizeof(inode_t))].size += write_size;
            write_blocks(inode_block_id, 1, inode_block);// Update inode block
            free(inode_block);                           // Free                                   (15)
         }
      }
      ptr_file_t *ptr_file = malloc(BLOCK_SIZE);// Malloc                                 (14)
      read_blocks(*i_ptr, 1, ptr_file);         // Retrieve pointer file

      ptr_file->ptrs[d_ptr_id - MAX_DIRECT_PTR] = new_block; // Update ptr
      write_blocks(*i_ptr, 1, ptr_file);        // Update pointer file
      free(ptr_file);                           // Free                                   (14)
      free(FBM);                                // Free                                   (16)

      return 0;
   }

   if(inode_id == -1) {                         // j-node: special procedure
      super_block_t *sb = calloc(BLOCK_SIZE, 1);                                          //11
      read_blocks(SUPER_BLOCK, 1, sb);
      
      sb->current_root.d_ptrs[d_ptr_id] = new_block;
      write_blocks(SUPER_BLOCK, 1, sb);
      free(sb);                                                                           //11
      fbm_t *FBM = malloc(BLOCK_SIZE);                                                    //12
      read_blocks(FBM_BLOCK, 1, FBM);
      FBM->mask[new_block] = 0;
      write_blocks(FBM_BLOCK, 1, FBM);          // Update FBM
      free(FBM);                                                                          //12
      inode->d_ptrs[d_ptr_id] = new_block;      // Don't forget to update the in-mem inode
   } else {                                     // normal i-node procedure
      // Getting the id of the inode table block that contains our inode.
      // Since int division truncates to 0, we do inode_id/number of inodes in a block
      b_ptr_t inode_block_id = get_block_id(&sb->current_root,inode_id/(BLOCK_SIZE/sizeof(inode_t)));

      if(inode_block_id == -1) return -1;
      inode->d_ptrs[d_ptr_id] = new_block;      // Don't forget to update the in-mem inode
      inode_t *inode_to_write_back = calloc(sizeof(inode_t), 1);                          //13
      *inode_to_write_back = *inode;
      inode_to_write_back->size += write_size;

      ssfs_fwseek(J_NODE, inode_id*sizeof(inode_t));
      ssfs_fwrite(J_NODE, (char*) inode_to_write_back, sizeof(inode_t));
      free(inode_to_write_back);                                                          //13
   }

   fbm_t *FBM = malloc(BLOCK_SIZE);
   read_blocks(FBM_BLOCK, 1, FBM);
   FBM->mask[new_block] = 0;
   write_blocks(FBM_BLOCK, 1, FBM);             // Update FBM
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
   for(int i=0; i<NUM_BLOCKS-65; i++) {
      if(fdt[i] == NULL) {
         fd_t *new_entry = calloc(sizeof(fd_t), 1);

         new_entry->inode = inode;
         new_entry->inode_id = inode_id;        
//       new_entry->read_ptr = { .d_ptr = 0, offset = 0 };// Unnecessary because of calloc
         new_entry->write_ptr = bytes_to_virt_addr(inode.size);

         fdt[i] = new_entry;
         return i;
      }
   }
   return -1;
}

b_ptr_t get_unused_block() {   // Gets an unused block (according to some strategy)
   wm_t *FBM = malloc(BLOCK_SIZE);
   read_blocks(FBM_BLOCK, 1, FBM);

   for(int i=0; i<BLOCK_SIZE; i++) {
      if(FBM->mask[i] == 1) {
         free(FBM);
         return i;
      }
   }
   free(FBM);
   return -1;
}

int get_free_inode() {          // Gets a free inode and returns its ID
   // Look into directory for the first gap. Pick that gap.
   inode_block_t *inode_block = calloc(BLOCK_SIZE, 1);
   ssfs_frseek(J_NODE, 0);                       // Read from beginning of j-node
   int d_ptr = 0;
   while(ssfs_fread(J_NODE, (char*) inode_block, BLOCK_SIZE) > 0) {
      for(int i=0; i<BLOCK_SIZE/sizeof(inode_t); i++) {
         if(inode_block->inodes[i].size == -1) {
            free(inode_block);
            return d_ptr*BLOCK_SIZE/sizeof(inode_t) + i; // Return ID
         }
      }
      d_ptr++;
   }
   free(inode_block);
   return -1;
}

int get_inode_id(char *name, super_block_t *sb) {
   int inode_id = -1;                              // ID of the inode in the inode list 
   virt_addr_t addr = { .d_ptr = 0, .offset = 0 }; // Offset is in bytes
   int num_entries = BLOCK_SIZE/DIR_ENTRY_SIZE;
   dir_t *dir_block = calloc(BLOCK_SIZE, 1);       // calloc                                  (6)

   while(addr.d_ptr*num_entries + addr.offset < fdt[ROOT_DIR]->inode.size) {
      ssfs_frseek(ROOT_DIR, virt_addr_to_bytes(addr));

      if(!(ssfs_fread(ROOT_DIR, (char*) dir_block, BLOCK_SIZE) > 0)) { // If read fails -> end of dir file
         break;
      }
      while(addr.offset<BLOCK_SIZE && addr.d_ptr*num_entries+addr.offset<fdt[ROOT_DIR]->inode.size) {
         if(strncmp(name, dir_block->files[addr.offset/DIR_ENTRY_SIZE].filename, FILENAME_SIZE) == 0) { // Compare filename
            inode_id = dir_block->files[addr.offset/DIR_ENTRY_SIZE].inode_id;
            break;
         }
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
