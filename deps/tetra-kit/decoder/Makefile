CC = g++
CFLAGS = -fmax-errors=5 -O2 -std=c++11 -Wall -Wextra
LDFLAGS = -lz

SRC = main.cc decoder.cc \
	common/base64.cc common/pdu.cc common/layer.cc common/log.cc common/report.cc common/tetracell.cc common/utils.cc common/tetra.cc \
	llc/llc.cc \
	mle/mle.cc mle/mle_elements.cc \
	sndcp/sndcp.cc \
	cmce/cmce.cc cmce/sds/sds.cc cmce/sds/lip.cc \
	mm/mm.cc mm/mm_elements.cc mm/d_authentication.cc mm/d_mm_status.cc mm/d_otar.cc \
	uplane/uplane.cc \
	mac/coding.cc mac/macdefrag.cc mac/mac.cc mac/viterbi.cc \
	wiremsg/wiremsg.cc

#SRC = 	decoder_main.cc coding.cc report.cc utils.cc viterbi.cc base64.cc \
#	tetra_dl.cc mac.cc llc.cc mle.cc cmce.cc cmce_sds.cc cmce_sds_lip.cc sndcp.cc \
#	uplane.cc mac_defrag.cc

OBJ = $(SRC:.cc=.o)
EXE = decoder

.cc.o:
	$(CC) $(CFLAGS) -c $< -o $@

all: $(EXE)

$(EXE): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDFLAGS)

test: test.o
	$(CC) $(CFLAGS) test.o -o $@ $(LDFLAGS)

clean:
	rm -f $(OBJ) $(EXE) *.o *~

final:
	make clean
	make
