# cimpmsg
socket server for parodus local communication

## Building
after you run cmake and make, you will get libcimpmsg.so in the build directory, and two test executables: cimpmsg_test_server and cimpmsg_test_client in the build/tests directory.

## Testing

. demo_server.sh

In another terminal window:

. demo_clients.sh

Demo shows a server receiving messages from 24 clients, and sending a hello message to each client.
Messages vary from 100 to 8000 bytes in length.

