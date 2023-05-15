
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

LIB_SRCS = array_rope.c bitmask_rope.c atthread_exit.c desc_tbl.c key.c thread_safe_global.c atomics.c
TEST_SRCS = t.c
SRCS = $(LIB_SRCS) $(TEST_SRCS)

LIB_OBJS := $(LIB_SRCS:%.c=%.o)
TEST_OBJS := $(TEST_SRCS:%.c=%.o)
OBJS := $(LIB_OBJS) $(TEST_OBJS)

depend: .depend

.depend: $(SRCS)
	rm -f "$@"
	$(CC) $(CFLAGS) -MM $^ > "$@"

include .depend

.c.o:
	$(CC) $(CFLAGS) -c $<

# XXX Add mapfile, don't export atomics -- or do export atomics but
# prefix them to avoid conflicts.
libtsgv.so: $(LIB_OBJS)
	$(CC) $(CSANFLAG) -shared -o libtsgv.so $(LDFLAGS) $(LDLIBS) $^

t: $(TEST_SRCS) libtsgv.so
	$(CC) $(CSANFLAG) -pie -o $@ $^ $(LDFLAGS) $(LDLIBS) -Wl,-rpath,$(PWD) -L$(PWD) -ltsgv

check: t
	./t

clean:
	rm -f t libtsgv.so $(OBJS) .depend
