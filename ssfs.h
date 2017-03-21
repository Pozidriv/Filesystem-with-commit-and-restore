// Shadow file system!!

void mkssfs(int fresh);

int ssfs_fopen(char*);
int ssfs_fclise(int);
int ssfs_frseek(int, int);
int ssfs_fwseek(int, int);
int ssfs_fwrite(int, char*, int);
int ssfs_fread(int, char*, int);
int ssfs_remove(char*);

int ssfs_commit();
int ssfs_restore(int);
