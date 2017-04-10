#include "sfs_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int get_option(void) {
   printf("Please select your option:\n");
   printf("(1) Open a new file\n");
   printf("(2) Write into a file\n");
   printf("(3) Read into a file\n");
   printf("(4) Close a file\n");
   printf("(5) Remove a file\n");
   printf("(6) Commit!\n");
   printf("(7) Restore to a previous version\n");

   int res = 0;
   scanf("%d", &res);
   return res;
}

void open(void) {
   printf("Please input the desired filename (less than 10 chars plz): \n");
   char str[11];
   scanf("%s", str);

   int num = ssfs_fopen(str);
   if(num < 0) {
      printf("Failed to open file. Possibly out of space.\n");
      return;
   }
   printf("File openend. File descriptor: %d\n", num);
}

void write(void) {
   int fd=0;
   char *str = malloc(1025);
   printf("File descriptor entry? ");
   scanf("%d", &fd);
   printf("String to write? (no whitespaces)\n");
   scanf("%s", str);

   int num = ssfs_fwrite(fd, str, strlen(str));
   free(str);
   if(num < 0) {
      printf("Failed to write to file. Possibly out of space.\n");
      return;
   }
   printf("Wrote %d bytes\n", num);
}

void read(void) {
   int fd=0;
   char *str = malloc(5000);
   printf("File descriptor entry? ");
   scanf("%d", &fd);

   ssfs_frseek(fd, 0);
   int num = ssfs_fread(fd, str, 4999);
   if(num < 0) {
      printf("Failed to write to file. Possibly out of space.\n");
      return;
   }
   str[num] = 0;
   printf("%s\n", str);
}

void close(void) {

}

void myremove(void) {

}

void commit(void) {
   printf("Commiting...\n");
   int num = ssfs_commit();
   if(num < 0) {
      printf("Failed to commit. Possibly out of space.\n");
      return;
   }
   printf("Commit complete. Previous commit's shadow root: %d\n", num);
}

void restore(void) {
   printf("What version do you want to restore to? ");
   int sr=0;
   scanf("%d", &sr);
   int num = ssfs_restore(sr);
   if(num < 0) {
      printf("Failed to restore. Are you sure shadow root number is valid?\n");
      return;
   }
   printf("Successfully restored\n");
}

int main(void) {
   printf("Welcome to Youri's personal test for commit and restore!\n");
   printf("Setting up a new filesystem...");
   mkssfs(1);
   printf(" Done\n\n");

   while(1) {
      switch(get_option()) {
         case 1:
            open();
            break;
         case 2:
            write();
            break;
         case 3:
            read();
            break;
         case 4:
            close();
            break;
         case 5:
            myremove();
            break;
         case 6:
            commit();
            break;
         case 7:
            restore();
            break;

         default:
            printf("I'm not sure what you meant by that... Try again\n");
      }
   }

   return 0;
}


