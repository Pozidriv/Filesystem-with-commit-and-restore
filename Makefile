# To compile with test1, make test1
# To compile with test2, make test2
CC = gcc -g -Wall
EXECUTABLE=sfs
EXECUTABLE2=sfs_gui

SOURCES_TEST1= disk_emu.c sfs_api.c sfs_test1.c tests.c
SOURCES_TEST2= disk_emu.c sfs_api.c sfs_test2.c tests.c
DEBUG= disk_emu.c sfs_api_debug.c sfs_test2.c tests.c
MYTEST= disk_emu.c sfs_api.c mytest.c
MYTESTDEBUG= disk_emu.c sfs_api_debug.c mytest.c

test1: $(SOURCES_TEST1) 
	$(CC) -o $(EXECUTABLE) $(SOURCES_TEST1)

test2: $(SOURCES_TEST2)
	$(CC) -o $(EXECUTABLE) $(SOURCES_TEST2)

debug: $(SOURCES_TEST2)
	$(CC) -o $(EXECUTABLE) $(DEBUG)

mytest: $(MYTEST)
	$(CC) -o $(EXECUTABLE2) $(MYTEST)

mytestdebug: $(MYTESTDEBUG)
	$(CC) -o $(EXECUTABLE2) $(MYTESTDEBUG)

clean:
	rm $(EXECUTABLE)
