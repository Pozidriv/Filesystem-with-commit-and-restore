//Functions you should implement. 
//Return -1 for error besides mkssfs
/*
typedef int b_ptr_t;                // Pointer to a disk block

struct ptr_file_t {                 // File that the indirect pointers point to
   b_ptr_t ptrs[BLOCK_SIZE/sizeof(b_ptr_t)];
};

struct inode_t {
   int size;                        // Size in bytes of the file (as in the used bytes)
   b_ptr_t d_ptrs[MAX_DIRECT_PTR];  // Direct pointers
   b_ptr_t i_ptr;                   // Indirect pointer
};

struct super_block_t {
   int magic;                       // Magic number
   int block_size;                  // 
   int num_blocks;                  //
   int num_inodes;                  //
   inode_t current_root;            //
   inode_t roots[NUM_SHADOW_ROOTS]; // Array of shadow roots
};

struct fbm_t {                      // File Bit Mask
   char mask[NUM_BLOCKS];           // Because ints are too big
};

struct wm_t {                       // Write Mask
   char mask[NUM_BLOCKS];
};

struct dir_entry_t {                // A directory entry (default: 16 bytes)
   char filename[FILENAME_SIZE+1];   // Null terminated?
   int inode_id;
};

struct dir_t {                      // A directory block (default: 64 entries)
   dir_entry_t files[BLOCK_SIZE/DIR_ENTRY_SIZE];
};
struct _inode_block_t {
   inode_t inodes[BLOCK_SIZE/sizeof(inode_t)];
} inode_block_t;
*/


void mkssfs(int fresh);
int ssfs_fopen(char *name);
int ssfs_fclose(int fileID);
int ssfs_frseek(int fileID, int loc);
int ssfs_fwseek(int fileID, int loc);
int ssfs_fwrite(int fileID, char *buf, int length);
int ssfs_fread(int fileID, char *buf, int length);
int ssfs_remove(char *file);
int ssfs_commit();
int ssfs_restore(int cnum);
