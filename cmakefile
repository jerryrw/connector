.PHONY: all nist clean sha3 sha256 md5 example test test_nist_md5 test_nist_sha256 test_nist_sha3 test_nist_aes256

OS = $(shell uname -s)
CC = gcc
CFLAGS = -std=c11 -Wall -O3 -march=native -mtune=native

all: sha3 sha256 md5 aes256 arcfour example test

# Test programs (NIST-like tests for MD5, SHA256, SHA3, SHA256)
test: test_nist_md5 test_nist_sha256 test_nist_sha3 test_nist_aes256 test_nist_arcfour
	@./test_nist_md5 || exit 2
	@./test_nist_sha256 || exit 2
	@./test_nist_sha3 || exit 2
	@./test_nist_aes256 || exit 2
	@./test_nist_arcfour || exit 2

# Build rules for tests
test_nist_md5: test_nist_md5.o md5
	$(CC) $(CFLAGS) test_nist_md5.o -o test_nist_md5 -L./ -lmd5 -Wl,-rpath,'$$ORIGIN'

test_nist_sha256: test_nist_sha256.o sha256
	$(CC) $(CFLAGS) test_nist_sha256.o -o test_nist_sha256 -L./ -lsha256 -Wl,-rpath,'$$ORIGIN'

test_nist_sha3: test_nist_sha3.o sha3
	$(CC) $(CFLAGS) test_nist_sha3.o -o test_nist_sha3 -L./ -lsha3 -Wl,-rpath,'$$ORIGIN'

test_nist_aes256: test_nist_aes256.o aes256
	$(CC) $(CFLAGS) test_nist_aes256.o -o test_nist_aes256 -L./ -laes256 -Wl,-rpath,'$$ORIGIN'

test_nist_arcfour: test_nist_arcfour.o arcfour
	$(CC) $(CFLAGS) test_nist_arcfour.o -o test_nist_arcfour -L./ -larcfour -Wl,-rpath,'$$ORIGIN'


test_nist_arcfour.o: test_nist_arcfour.c
	$(CC) $(CFLAGS) -c test_nist_arcfour.c -o test_nist_arcfour.o

test_nist_md5.o: test_nist_md5.c
	$(CC) $(CFLAGS) -c test_nist_md5.c -o test_nist_md5.o

test_nist_sha256.o: test_nist_sha256.c
	$(CC) $(CFLAGS) -c test_nist_sha256.c -o test_nist_sha256.o

test_nist_sha3.o: test_nist_sha3.c
	$(CC) $(CFLAGS) -c test_nist_sha3.c -o test_nist_sha3.o

test_nist_aes256.o : test_nist_aes256.c
	$(CC) $(CFLAGS) -c test_nist_aes256.c -o test_nist_aes256.o

example: example.o sha3 sha256 md5 aes256 arcfour
	$(CC) $(CFLAGS) example.o -o example -L./ -lsha3 -lsha256 -lmd5 -laes256 -Wl,-rpath,'$$ORIGIN'

example.o: example.c
	$(CC) $(CFLAGS) -c example.c -o example.o

sha3: sha3.o
ifeq ($(OS),Linux)
    # Commands and variables specific to Linux
	$(CC) -D_GNU_SOURCE sha3.o -o libsha3.so -shared -fPIC -ldl
else ifeq ($(OS),Darwin) # macOS
    # Commands and variables specific to macOS
	$(CC) -dynamiclib -exported_symbols_list symbols/libsha3.exp sha3.o -o libsha3.dylib 
endif

sha256: sha256.o
ifeq ($(OS),Linux)
    # Commands and variables specific to Linux
	$(CC) -D_GNU_SOURCE sha256.o -o libsha256.so -fPIC -shared -ldl
else ifeq ($(OS),Darwin) # macOS
    # Commands and variables specific to macOS
	$(CC) -dynamiclib -exported_symbols_list symbols/libsha256.exp sha256.o -o libsha256.dylib
endif

md5: md5.o
ifeq ($(OS),Linux)
    # Commands and variables specific to Linux
	$(CC) -D_GNU_SOURCE md5.o -o libmd5.so -fPIC -shared -ldl	
else ifeq ($(OS),Darwin) # macOS
    # Commands and variables specific to macOS
	$(CC) -dynamiclib -exported_symbols_list symbols/libmd5.exp md5.o -o libmd5.dylib
endif

aes256: aes256.o
ifeq ($(OS),Linux)
    # Commands and variables specific to Linux
	$(CC) -D_GNU_SOURCE aes256.o -o libaes256.so -fPIC -shared -ldl	
else ifeq ($(OS),Darwin) # macOS
    # Commands and variables specific to macOS
	$(CC) -dynamiclib -exported_symbols_list symbols/libaes256.exp aes256.o -o libaes256.dylib
endif

arcfour: arcfour.o
ifeq ($(OS),Linux)
    # Commands and variables specific to Linux
	$(CC) -D_GNU_SOURCE arcfour.o -o libarcfour.so -fPIC -shared -ldl	
else ifeq ($(OS),Darwin) # macOS
    # Commands and variables specific to macOS
	$(CC) -dynamiclib -exported_symbols_list symbols/libarcfour.exp arcfour.o -o libarcfour.dylib
endif

sha3.o: sha3_256.c
ifeq ($(OS),Linux)
	# Commands and variables specific to Linux
	$(CC) $(CFLAGS) -D_GNU_SOURCE -c sha3_256.c -o sha3.o -fPIC -ldl  
else ifeq ($(OS),Darwin) # macOS
	# Commands and variables specific to macOS
	$(CC) $(CFLAGS) -c sha3_256.c -o sha3.o 
endif

sha256.o: sha256.c
ifeq ($(OS),Linux)
	# Commands and variables specific to Linux
	$(CC) $(CFLAGS) -D_GNU_SOURCE -c sha256.c -o sha256.o -fPIC -ldl  
else ifeq ($(OS),Darwin) # macOS
	# Commands and variables specific to macOS
	$(CC) $(CFLAGS) -c sha256.c -o sha256.o
endif

md5.o: md5.c
ifeq ($(OS),Linux)
	# Commands and variables specific to Linux
		$(CC) $(CFLAGS) -D_GNU_SOURCE -c md5.c -o md5.o -fPIC -ldl  

else ifeq ($(OS),Darwin) # macOS
	# Commands and variables specific to macOS
	$(CC) $(CFLAGS) -c md5.c -o md5.o
endif

aes256.o: aes256.c
	$(CC) $(CFLAGS) -c aes256.c -o aes256.o

arcfour.o: arcfour.c
	$(CC) $(CFLAGS) -c arcfour.c -o arcfour.o

clean:
	rm -f *.o *.so example *.dylib test_nist_md5 test_nist_sha256 test_nist_sha3 test_nist_aes256 test_nist_arcfour


#ifeq ($(OS),Linux)
    # Commands and variables specific to Linux
#else ifeq ($(OS),Darwin) # macOS
    # Commands and variables specific to macOS