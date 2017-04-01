
void mkssfs(int fresh);                   // Creates the file system

int ssfs_fopen(char*);                    // Opens the given file
int ssfs_fclise(int);                     // Closes the given file
int ssfs_frseek(int, int);                // Seeks (Read) to the location from beginning
int ssfs_fwseek(int, int);                // Seeks (Write) to the location from beginning
int ssfs_fwrite(int, char*, int);         // Write characters in buf to disk
int ssfs_fread(int, char*, int);          // Read characters from disk into buf
int ssfs_remove(char*);                   // Removes a file from the filesystem

int ssfs_commit();                        // Creates a shadow of the filesystem
int ssfs_restore(int);                    // REstpres the file system to a previous shadow
