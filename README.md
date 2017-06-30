# OS_Project
A simple shadow filesystem that I implemented for my COMP 310 class. 
Should work well. Commit and restore seems to work, but no guarantees on that.

To compile for a baisc test: 

```make test1```

To compile for a stress test: 

```make test2```

To compile for an interactive test: 

```make mytest```


There is one edge case where the filesystem might have undefined behavior:
When doing commit and restore of files large enough to use a block of pointers


For more info or if you find any bugs, contact me: youri.tamitegama@mail.mcgill.ca
