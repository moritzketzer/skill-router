# skillrouter - consolidated skill-library interface/router.
# Stdlib + statically-linked SQLite amalgamation only. No package-manager
# dependencies: third_party/sqlite3.{h,c} is vendored, so this builds
# identically on any machine with a C++20 compiler.

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -I.
CC       ?= gcc
SQLITE_DEFS = -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5

MINGW_CXX ?= x86_64-w64-mingw32-g++
MINGW_CC  ?= x86_64-w64-mingw32-gcc

.PHONY: all test clean windows

all: skillrouter

# sqlite3.c is C, not C++ (uses `new` as an identifier, implicit void*
# conversions) -- it MUST be compiled by a C compiler into its own object
# file and linked, never #included into a C++ translation unit.
sqlite3.o: third_party/sqlite3.c
	$(CC) -O2 -c third_party/sqlite3.c -o sqlite3.o $(SQLITE_DEFS)

skillrouter: main.cpp skill_library.hpp mcp_server.hpp sqlite3.o
	$(CXX) $(CXXFLAGS) main.cpp sqlite3.o -o $@ -lpthread -ldl

test_library: test_library.cpp skill_library.hpp mcp_server.hpp sqlite3.o
	$(CXX) $(CXXFLAGS) test_library.cpp sqlite3.o -o $@ -lpthread -ldl

test: test_library
	./test_library

# Genuine cross-compiled Windows binary: statically linked (no external DLL
# needed beyond system ws2_32), verified functional under Wine during
# development (index/search/fetch/stats all confirmed working).
sqlite3_win.o: third_party/sqlite3.c
	$(MINGW_CC) -O2 -c third_party/sqlite3.c -o sqlite3_win.o $(SQLITE_DEFS)

windows: skillrouter.exe

skillrouter.exe: main.cpp skill_library.hpp mcp_server.hpp sqlite3_win.o
	$(MINGW_CXX) -std=c++20 -O2 -static -static-libgcc -static-libstdc++ \
	    -I. main.cpp sqlite3_win.o -o $@ -lws2_32

clean:
	rm -f skillrouter skillrouter.exe test_library sqlite3.o sqlite3_win.o *.db *.db-wal *.db-shm
