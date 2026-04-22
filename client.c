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

typedef enum MessageType { 
	LOGIN,
	LOGOUT,
	MESSAGE_SEND,
	MESSAGE_RECV=10,
	DISCONNECT=12,
	SYSTEM
} message_type_t;

typedef struct __attribute__((packed)) Message { 
	unsigned int message_type;
	unsigned int timestamp;
	char username[32];
	char message[1024];
} message_t;

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

	settings.server.sin_family = AF_INET;
	sa_family_t address_family = settings.server.sin_family;

	// parsing arguments
	for (int i = 1; i < argc; i++) {
		
		if (strcmp(argv[i], "--help") == 0) { // processing help argument

			print_help();
			exit(1);

		} else if (strcmp(argv[i], "--port") == 0) { // processing the port

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

	// reading the username from the whoami command
	if (fgets(settings.username, sizeof(settings.username), username) == NULL){
		perror("Error: could not get username from whoami");
		pclose(username);
		return -1;
	}

	// empty username error
	if (settings.username[0] == '\0'){
		perror("Error: username is empty");
		pclose(username);
		return -1;
	}

	// checking that each character in the username is printable
	
	int length = strlen(settings.username);

	// removing the new line from the username and replacing it with a null terminator
	settings.username[length-1] = '\0';

	for (int i = 0; i < strlen(settings.username); i++) {

		// non-printable username error
		if (!isprint(settings.username[i])){
			fprintf(stderr, "Error: nonprintable username");
			pclose(username);
			return -1;
		}

	}

    pclose(username);
    username = NULL;
    return 0;
}

void handle_signal(int signal) {
    
}

ssize_t perform_full_read(int fd, void *buf, size_t n) {
	size_t remaining = n;
	char *curr_position = (char *) buf;

	while (remaining > 0) {
		
		// attempting full read
		ssize_t bytes_read = read(fd, curr_position, remaining);

		if (bytes_read == 0) {
			fprintf(stderr, "server closed connection");
			return 0;
		}

		if (bytes_read < 0) {
			perror("Error: could not read incoming message");
			return -1;
		}

		// moving position to where the short read stopped
		curr_position += bytes_read;
		remaining -= bytes_read;
	}

	return n;
}

void* receive_messages_thread(void* arg) {

    int socket = (int)(intptr_t)arg;
    message_t inbound_message;

    printf("receiving\n");
    while(settings.running){
 
        // read message from the server (ensure no short reads)
	if (perform_full_read(socket, &inbound_message, sizeof(inbound_message)) <= 0) {
		settings.running = false;
		break;
	}

	switch(inbound_message.message_type){
		case MESSAGE_RECV:{

			// converting timestamp into a readable date format
			char time_buffer[50];
			time_t time = (time_t) inbound_message.timestamp;
			struct tm *structured_time = localtime(&time);

			// converting into date format and storing it
			strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", structured_time);

			// printing the date and sender username
			printf("[%s] %s: ", time_buffer, inbound_message.username);

			// searching for mentions and highlighting

			char *mention_position;
			char *current_position = inbound_message.message;

			// creating the mention format to search for
			char mention_format[40];
			snprintf(mention_format, sizeof(mention_format), "@%s", settings.username);

			// searching for mentions
			while ((mention_position = strstr(current_position, mention_format)) != NULL){
		
				// print everything before the mention
				fprintf(stdout, "%.*s ", (int) (mention_position - current_position), current_position);
				
				// the mention with red highlight and ascii bell
				fprintf(stdout, "\a\033[31m%s\033[0m ", mention_format);

				current_position = mention_position + strlen(mention_format);
			}

			// printing the rest of the message
			fprintf(stdout, "%s\n", current_position);
			break;
		}

		case SYSTEM:{
			fprintf(stdout, "\033[90m[SYSTEM] %s logged in\033[0m", settings.username);
			break;
		}
	}
            // for message types, print the message and do highlight parsing (if not quiet)
            // for system types, print the message in gray with username SYSTEM
            // for disconnect types, print the reason in red with username DISCONNECT and exit
            // for anything else, print an error
	fflush(stdout);
    }
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

    if (get_username_status == -1) {
	    perror("Error: could not get username");
	    return -1;
    }
    // create socket

   int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd == -1) {
	    perror("Error: could not create socket");
	    close(sockfd);
	    sockfd = -1;
	    return -1;
    }

    // connect to server

    int connect_status = connect(sockfd, (struct sockaddr *)&settings.server, sizeof(settings.server));
    if (connect_status == -1) {
	    perror("Error: could not connect to address");
	    close(sockfd);
	    sockfd = -1;
	    return -1;
    }

    printf("Connected\n");
    // create and send login message

    message_t login_msg = {0};
    

    // setting the message type 
    login_msg.message_type = LOGIN;

    login_msg.username[sizeof(login_msg.username)-1] = '\0'; // null terminating the username manually just to make sure

    // setting the username
    strncpy(login_msg.username, settings.username, sizeof(settings.username)-1);

    // sending the message to server
    int send_login_message = write(sockfd, &login_msg, sizeof(login_msg));

    if (send_login_message < sizeof(login_msg)) {
	    perror("Error: could not send login message");
	    close(sockfd);
	    sockfd = -1;
	    return -1;
    }

    // create and start receive messages thread
    pthread_t receive_thread;

    if((pthread_create(&receive_thread, NULL, receive_messages_thread, (void *) (intptr_t) sockfd)) != 0) {
	    perror("Error: could not create receive thread");
	    close(sockfd);
	    return -1;
    }

    settings.running = true;
 
    // while some condition(s) are true
        // read a line from STDIN
        // do some error checking (handle EOF, EINTR, etc.)
        // send message to the server
    
    // wait for the thread / clean up

    // cleanup and return
}
