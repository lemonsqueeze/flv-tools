

PROG=flv_cut flv_fix_seek flv_merge flv_debug flv_fix

#CFLAGS=-g -Wall
CFLAGS=-O2 -Wall

all: $(PROG)


clean:
	-rm $(PROG) *~

