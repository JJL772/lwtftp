
all:
	$(CC) -o tftpd -g -O0 tftpd.c

clean:
	rm tftpd