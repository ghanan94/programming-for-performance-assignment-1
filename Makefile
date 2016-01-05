CFLAGS?=-std=c99 -D_GNU_SOURCE -Wall -O2 -g

default: all

all: bin bin/paster bin/paster_parallel bin/paster_nbio report

report: report.pdf

bin:
	mkdir bin

bin/paster: src/paster.c
	$(CC) $< $(CFLAGS) -o bin/paster -lpng -lcurl

bin/paster_parallel: src/paster_parallel.c
	$(CC) $< $(CFLAGS) -DPARALLEL -pthread -lpng -lcurl -o bin/paster_parallel

bin/paster_nbio: src/paster_nbio.c
	$(CC) $< $(CFLAGS) -DPARALLEL -lpng -lcurl -o bin/paster_nbio

report.pdf: report/report.tex
	cd report && pdflatex report.tex && pdflatex report.tex
	mv report/report.pdf report.pdf

clean:
	rm -r bin
