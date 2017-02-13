This project implemented a client and a server in Unix environment.

**<h3><ins>Makefile (needs OpenSSL library):</ins></h3>**
make tserver&nbsp;&nbsp;&nbsp;&nbsp;// *create the executable file tserver*<br/>
make tclient&nbsp;&nbsp;&nbsp;&nbsp;// *create the executable file tclient*

**<h3><ins>The server creates:</ins></h3>**
a TCP/UDP stream socket in the Internet domain bounding to a port number (specified as a commandline argument), 
receives requests from clients, produces proper results based on the content of requests and responds back to the client 
on the same connection.

**<h3><ins>The requests a client can send:</ins></h3>**
***file type request:*** a request to get the file type of a file on the server<br/> 
***file checksum request:*** a request to get the checksum of a file on the server<br/> 
***download file request:*** a request to download a file from the server<br/> 

**<h3><ins>The commandline syntax:</ins></h3>**
tserver [-udp loss_model [-w window] [-r msinterval]] [-d] [-t seconds] port // *starts the server*<br/>
tclient [hostname:]port filetype [-udp] filename // *client sends filetype request*<br/>
tclient [hostname:]port checksum [-udp] [-o offset] [-l length] filename // *client sends checksum request*<br/>
tclient [hostname:]port download [-udp] [-o offset] [-l length] filename [saveasfilename] // *client sends download file request*<br/>

**<h3><ins>Commandline syntax illustration:</ins></h3>**
The ***loss_model*** names a binary file which is read one byte at a time. When a bit is needed to determine 
whether to throw a packet away or not, look at the left-most bit in the byte that has not been examined. 
Once all 8 bits have been used in a byte, read the next byte. 
If the end of the loss model file has been reached, start from the beginning of the file again.

A window-based protocol is used to provide reliability where ***window*** is the size of the congestion window (must be ≥ 1) 
and ***msinterval*** is the timeout interval in milliseconds (must be ≥ 1 and ≤ 5000). If the ***-w*** commandline option is not specified, the default window size is 3. If the ***-r*** commandline option is not specified, the default timeout interval is 250.

***-d:*** debug mode<br/>
***-t seconds:*** server auto-shutdown time; must be ≥ 5; default 300 seconds<br/>
***-o offset:*** for network byte order format; must be >= 0; default 0<br/>
***-l length:*** for network byte order format; must be >= 0; default 0xffffffff (-1)<br/>

**<h3><ins>The port number range:</ins></h3>**
10000 to 65535

**<h3><ins>About the UDP mode:</ins></h3>**
<p><b><i>*The real UDP does not resend missed packets! This design is just for the purpose of practice</i></b></p>
For the server, it uses a "loss model" to simulate that UDP packets are dropped. The loss model works by reading bits from a loss model file. Every time an UDP packet from the server to the client is ready to send, read a bit from that file. If the bit is a one, the server sends the UDP packet. If the bit is a zero, the server does not send the UDP packet (and pretend that the packet was lost somewhere in the middle of the Internet). The server only retransmits a loss packet after a timeout interval has expired.

For the client, once it receives a UDP packet, it responses an ACK packet. The first four bytes in ACK packet is the same as the first four bytes in UDP packet. When the congestion window becomes full or all packets have been sent, the server sleeps for a while then checks if it received ACKs from client.

The maximum allowed UDP packet size is 4,096 bytes. The server applies buffers that have size 4096 bytes. The number of buffers depends on the window size. When the server has a long message to send, it breaks up the message into multiple UDP packets including a 8 bytes sequence number at the header and a 4,088 bytes long frame. The UDP packets from the client to the server also follow the same format.
