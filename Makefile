all:
	$(CROSS_COMPILE)gcc -Wall -o droid4-ngsm droid4-ngsm.c

clean:
	rm -f droid4-ngsm
