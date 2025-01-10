
all:
	$(CC) -o tftpd -g -O1 -Wall tftpd.c

clean:
	rm tftpd
