
CC = gcc
LD = ld

ifeq ($(CC),gcc)
    CWARNFLAGS = -Wall -Wextra
else ifeq ($(CC),clang)
    CWARNFLAGS = -Wall -Wextra
endif

COPTFLAG = -O0
CDBGFLAG = -ggdb3
CSANFLAG = -fsanitize=undefined -fsanitize=thread

# Atomics backends: -DHAVE___ATOMIC,
# 		    -DHAVE___SYNC,
# 		    Win32 (do not set/define),
#                   -DHAVE_INTEL_INTRINSICS,
#                   -DHAVE_PTHREAD,
#                   -DNO_THREADS
ATOMICS_BACKEND = -DHAVE___ATOMIC

# Implementations:  -DUSE_TSV_SLOT_PAIR_DESIGN (default),
# 		    -DUSE_TSV_SUBSCRIPTION_SLOTS_DESIGN
TSV_IMPLEMENTATION = 

CPPDEFS = 
CPPFLAGS = $(ATOMICS_BACKEND) $(TSV_IMPLEMENTATION)
CFLAGS = -fPIC $(CSANFLAG) $(CDBGFLAG) $(COPTFLAG) $(CWARNFLAGS) $(CPPFLAGS) $(CPPDEFS)

LDLIBS =  -lpthread -lrt #(but not on Windows, natch)
LDFLAGS =

slotpair : TSV_IMPLEMENTATION = -DUSE_TSV_SLOT_PAIR_DESIGN
slotpair : t

slotlist : TSV_IMPLEMENTATION = -DUSE_TSV_SUBSCRIPTION_SLOTS_DESIGN
slotlist : t

slotpairO0 : COPTFLAG = -O0
slotpairO0 : slotpair
slotpairO1 : COPTFLAG = -O1
slotpairO1 : slotpair
slotpairO2 : COPTFLAG = -O2
slotpairO2 : slotpair
slotpairO3 : COPTFLAG = -O3
slotpairO3 : slotpair

slotlistO0 : COPTFLAG = -O0
slotlistO0 : slotlist
slotlistO1 : COPTFLAG = -O1
slotlistO1 : slotlist
slotlistO2 : COPTFLAG = -O2
slotlistO2 : slotlist
slotlistO3 : COPTFLAG = -O3
slotlistO3 : slotlist

.c.o:
	$(CC) $(CFLAGS) -c $<

# XXX Add mapfile, don't export atomics
libtsgv.so: array_rope.o bitmask_rope.o atthread_exit.o desc_tbl.o key.o thread_safe_global.o atomics.o
	$(CC) $(CSANFLAG) -shared -o libtsgv.so $(LDFLAGS) $(LDLIBS) $^

t: t.o libtsgv.so
	$(CC) $(CSANFLAG) -pie -o $@ $^ $(LDFLAGS) $(LDLIBS) -Wl,-rpath,$(PWD) -L$(PWD) -ltsgv

clean:
	rm -f t t.o libtsgv.so thread_safe_global.o atomics.o array_rope.o bitmask_rope.o atthread_exit.o desc_tbl.o key.o
