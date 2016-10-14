all:tserver tclient
tserver:tserver.c
	gcc tserver.c -o tserver -lssl -lcrypto -lnsl -g -lsocket -I/home/scf-22/csci551b/openssl/include
tclient:tclient.c
	gcc tclient.c -o tclient -lssl -lcrypto -lnsl -g -lsocket -I/home/scf-22/csci551b/openssl/include
clean:
	rm -rf tserver tclient
