
CC = gcc
LD = ld

ifeq ($(CC),gcc)
    CWARNFLAGS = -Wall -Wextra
else ifeq ($(CC),clang)
    CWARNFLAGS = -Wall -Wextra
endif

COPTFLAG = -O0
CDBGFLAG = -ggdb3

# Atomics backends: -DHAVE___ATOMIC,
# 		    -DHAVE___SYNC,
# 		    Win32 (do not set/define),
#                   -DHAVE_INTEL_INTRINSICS,
#                   -DHAVE_PTHREAD,
#                   -DNO_THREADS
ATOMICS_BACKEND = -DHAVE___ATOMIC

# Implementations:  -DUSE_TSGV_SLOT_PAIR_DESIGN (default),
# 		    -DUSE_TSGV_SUBSCRIPTION_SLOTS_DESIGN
TSGV_IMPLEMENTATION = 

CPPFLAGS = $(ATOMICS_BACKEND) $(TSGV_IMPLEMENTATION)
CFLAGS = -fPIC $(CDBGFLAG) $(COPTFLAG) $(CWARNFLAGS) $(CPPFLAGS)

LDLIBS = -lpthread #(but not on Windows, natch)
LDFLAGS = 

slotpair : TSGV_IMPLEMENTATION = -DUSE_TSGV_SLOT_PAIR_DESIGN
slotpair : t

slotlist : TSGV_IMPLEMENTATION = -DUSE_TSGV_SUBSCRIPTION_SLOTS_DESIGN
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
	$(CC) -pie $(CFLAGS) -c $<

# XXX Add mapfile, don't export atomics
libtsgv.so: thread_safe_global.o atomics.o
	$(LD) -shared -o libtsgv.so $^

t: t.o thread_safe_global.o atomics.o
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f t t.o libtsgv.so thread_safe_global.o atomics.o
