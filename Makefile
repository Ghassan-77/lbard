EXECS = lbard manifesttest fakecsmaradio

all:	$(EXECS)

clean:
	rm -rf version.h $(EXECS) echotest

SRCS=	main.c rhizome.c txmessages.c rxmessages.c bundle_cache.c json.c peers.c \
	serial.c radio.c golay.c httpclient.c progress.c rank.c bundles.c partials.c \
	manifests.c monitor.c timesync.c httpd.c meshms.c \
	energy_experiment.c status_dump.c \
	fec-3.0.1/ccsds_tables.c \
	fec-3.0.1/encode_rs_8.c \
	fec-3.0.1/init_rs_char.c \
	fec-3.0.1/decode_rs_8.c \
	bundle_tree.c sha1.c sync.c \
	hfcontroller.c uhfcontroller.c \
	eeprom.c sha3.c otaupdate.c

HDRS=	lbard.h serial.h Makefile version.h sync.h sha3.h miniz.c
#CC=/usr/local/Cellar/llvm/3.6.2/bin/clang
#LDFLAGS= -lgmalloc
#CFLAGS= -fno-omit-frame-pointer -fsanitize=address
#CC=clang
#LDFLAGS= -lefence
LDFLAGS=
CFLAGS= -g -std=gnu99 -Wall -fno-omit-frame-pointer -D_GNU_SOURCE=1

version.h:	$(SRCS) $(HDRS)
	echo "#define VERSION_STRING \""`./md5 $(SRCS)`"\"" >version.h
	echo "#define GIT_VERSION_STRING \""`git describe --always --abbrev=10 --dirty=+DIRTY`"\"" >>version.h
	echo "#define GIT_BRANCH \""`git rev-parse --abbrev-ref HEAD`"\"" >>version.h
	echo "#define BUILD_DATE \""`date`"\"" >>version.h

lbard:	$(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o lbard $(SRCS) $(LDFLAGS)

echotest:	Makefile echotest.c
	$(CC) $(CFLAGS) -o echotest echotest.c

FAKERADIOSRCS=	fakecsmaradio.c \
		fake_rfd900.c \
		fake_hfcodan.c \
                fake_hfbarrett.c\
		fec-3.0.1/ccsds_tables.c \
		fec-3.0.1/encode_rs_8.c \
		fec-3.0.1/init_rs_char.c \
		fec-3.0.1/decode_rs_8.c
fakecsmaradio:	Makefile $(FAKERADIOSRCS) fakecsmaradio.h
	$(CC) $(CFLAGS) -o fakecsmaradio $(FAKERADIOSRCS)

manifesttest:	Makefile manifests.c
	$(CC) $(CFLAGS) -DTEST -o manifesttest manifests.c
