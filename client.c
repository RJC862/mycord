#include <stdbool.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <stdint.h>

// typedef enum MessageType { ... } message_type_t;
// typedef struct __attribute__((packed)) Message { ... } message_t;
typedef struct Settings {
    struct sockaddr_in server;
    bool quiet;
    int socket_fd;
    bool running;
    char username[32];
} settings_t;

static char* COLOR_RED = "\033[31m";
static char* COLOR_GRAY = "\033[90m";
static char* COLOR_RESET = "\033[0m";
static settings_t settings = {0};

void print_help(void){
	// prints the help menu
	
	printf("usage: ./client [-h] [--port PORT] [--ip IP] [--domain DOMAIN] [--quiet]\n\n"

		"mycord client\n\n"

		"options:\n"
  		"--help                show this help message and exit\n"
  		"--port PORT           port to connect to (default: 8080)\n"
 		"--ip IP               IP to connect to (default: \"127.0.0.1\")\n"
  		"--domain DOMAIN       Domain name to connect to (if domain is specified, IP must not be)\n"
  		"--quiet               do not perform alerts or mention highlighting\n\n"

		"examples:\n"
  		"/client --help (prints the above message)\n"
  		"/client --port 1738 (connects to a mycord server at 127.0.0.1:1738)\n"
  		"/client --domain example.com (connects to a mycord server at example.com:8080)\n");

}

int process_args(int argc, char *argv[]) {

	bool ip_specified = false;
	bool domain_specified = false;

	sa_family_t address_family = settings.server.sin_family;

	// parsing arguments
	for (int i = 1; i < argc; i++) {
		
		if (strcmp(argv[i], "--help") == 0) { // processing help argument

			print_help();
			return 0;

		} else if (strcmp(argv[i], "port") == 0) { // processing the port

			if (i + 1 < argc) {

				// setting the port number

				// casting the port into an integer type so it can be cast into an unsigned short
				unsigned long port_number = strtoul(argv[i+1], NULL, 10);

				if (port_number > 65535) {
					perror("Error: invalid port number");
					return -1;
				}

				// assigning the port number with htons
				settings.server.sin_port = htons((uint16_t) port_number);

				// skipping past the port number since it was processed
				i++;

			} else {

				perror("Missing port number");
				return -1;

			}
		} else if (strcmp(argv[i], "ip") == 0) { // processing ip
			
			ip_specified = true;

			if (i + 1 < argc){

				// setting the IP
				int inet_pton_status = inet_pton(address_family, argv[i+1], &settings.server.sin_addr); 
				
				if (inet_pton_status != 1) {
					
					perror("Error: invalid IP address");
					return -1;

				} 

				// skipping past the ip address since it was processed
				i++;

			} else {

				// setting the default ip address
				inet_pton(address_family, "127.0.0.1", &settings.server.sin_addr);

			}

		} else if (strcmp(argv[i], "--domain") == 0 ) { // processing domain name

			if (i + 1 < argc) {

				domain_specified = true;

				// setting the ip address with DNS

				struct hostent *hostinfo;

				if ((hostinfo = gethostbyname(argv[i+1])) == NULL) {

					// error: could not find host name with DNS
					perror("Error: could not find host name with DNS");
					return -1;

				} else {
					
					// setting the ip address to the first in the list
					inet_pton(address_family, hostinfo->h_addr_list[0], &settings.server.sin_addr);
				}

				// skip past the DNS address since it was processed
				i++;
			} else {
				
				// do nothing if there is no domain specified
				continue;
			}

		} else if (strcmp(argv[i], "--quiet") == 0 ) {

			// suppress alerts and mention highlighting
			settings.quiet = true;

		} else {

			// error: invalid argument
			perror("Error: invalid argument");
			return -1;
		}
	}

	if (ip_specified && domain_specified) {

		// error: cannot specify both ip and domain, either one or the other
		fprintf(stderr, "Error: cannot specify both ip and domain, either one or the other");
		return -1;	
	}

    // successful argument processing
    return 0;
}

int get_username() {

	FILE *username;

	// running the whoami command
	username = popen("whoami", "r");

	if (username == NULL) {
		return -1;
	}

	int fd = fileno(username);

	int bytes_read = 0;

	// reading the username from the whoami command
	if ((bytes_read = read(fd, settings.username, sizeof(settings.username))) == -1){

		perror("Error: could not get username");
		pclose(username);
		return -1;
	}

	// no username error
	if (bytes_read == 0) {
		perror("Error: username not found");
		pclose(username);
		return -1;
	}

	// invalid username error
	if (settings.username == NULL || settings.username[0] == '\0'){
		perror("Error: invalid username");
		pclose(username);
		return -1;
	}

	for (int i = 0; i < bytes_read; i++) {

		// non-printable username error
		if (!isprint(settings.username[i])){
			perror("Error: invalid username");
			pclose(username);
			return -1;
		}

	}

    pclose(username);
    return 0;
}

void handle_signal(int signal) {
    
}

ssize_t perform_full_read(void *buf, size_t n) {
    return -1;
}

void* receive_messages_thread(void* arg) {
    // while some condition(s) are true
        // read message from the server (ensure no short reads)
        // check the message type
            // for message types, print the message and do highlight parsing (if not quiet)
            // for system types, print the message in gray with username SYSTEM
            // for disconnect types, print the reason in red with username DISCONNECT and exit
            // for anything else, print an error
}

int main(int argc, char *argv[]) {
    // setup sigactions (ill-advised to use signal for this project, use sigaction with default (0) flags instead)
	
    
    // parse arguments

    int process_args_status = process_args(argc, argv);

    if (process_args_status == -1) {
	    // error: could not process arguments
	    return -1;
    }

    // get username

    int get_username_status = get_username();
    // create socket

    // connect to server

    // create and send login message

    // create and start receive messages thread

    // while some condition(s) are true
        // read a line from STDIN
        // do some error checking (handle EOF, EINTR, etc.)
        // send message to the server
    
    // wait for the thread / clean up

    // cleanup and return
}
