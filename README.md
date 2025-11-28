# Server-to-Client-Chatroom
A C program to run a chatroom between a network through multiple computers/terminals
## To run:
1) Run command "$ gcc -pthread chat_server.c -o chat_server"
2) Run command "$ gcc -pthread chat_server.c -o chat_server"
3) Open two more ssh terminals to have 3 terminals total
4) Run command $ "./chat_server on one terminal"
5) On the other two terminals, run these commands if all 3 are on the same ssh host "$ ./chat_client 127.0.0.1 4267"
6) If a terminal is off the host's ssh server, instead of using the IP 127.0.0.1, use the host's IP address instead. To find that, the host will give you the IP (if they trust you enough) through typing "$ hostname -I".
## Quitting:
- Clients can quit using ctrl+D
- Host shutdown server using ctrl+C