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
    bool disconnected;
    int socket_fd;
    bool running;
    char username[32];
} settings_t;

static char* COLOR_RED = "\033[31m";
static char* COLOR_GRAY = "\033[90m";
static char* COLOR_RESET = "\033[0m";
static settings_t settings = {0};

volatile sig_atomic_t signal_status = 0;

static const char HELP_MSG[] = 	
		"usage: ./client [-h] [--port PORT] [--ip IP] [--domain DOMAIN] [--quiet]\n\n"

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
  		"/client --domain example.com (connects to a mycord server at example.com:8080)\n";

void print_help(void){
	// prints help menu
	
	fputs(HELP_MSG, stdout);

}

int process_args(int argc, char *argv[]) {

	bool ip_specified = false;
	bool domain_specified = false;

	settings.server.sin_family = AF_INET;
	settings.server.sin_port = htons(8080);
	inet_pton(AF_INET, "127.0.0.1", &settings.server.sin_addr);

	// parsing arguments
	for (int i = 1; i < argc; i++) {
		
		if (strcmp(argv[i], "--help") == 0) { // processing help argument

			print_help();
			exit(0);

		} else if (strcmp(argv[i], "--port") == 0) { // processing the port

			if (i + 1 < argc) {

				// setting the port number

				char *endptr;

				unsigned long port_number = strtoul(argv[i+1], &endptr, 10);

				if (*endptr != '\0' || port_number > 65535) {
					fprintf(stderr, "Error: invalid port number\n");
					return -1;
				}

				// assigning the port number with htons
				settings.server.sin_port = htons((uint16_t) port_number);

				// skipping past the port number since it was processed
				i++;
			} else {
				fprintf(stderr, "Error: missing port number\n");
				return -1;
			}
		} else if (strcmp(argv[i], "--ip") == 0) { // processing ip
		
			if (domain_specified) {
				fprintf(stderr, "Error: cannot specify both ip and domain, either one or the other\n");
				return -1;
			}	
			ip_specified = true;

			if (i + 1 < argc){

				// setting the IP
				int inet_pton_status = inet_pton(AF_INET, argv[i+1], &settings.server.sin_addr); 
				
				if (inet_pton_status != 1) {
					
					fprintf(stderr,"Error: invalid IP address\n");
					return -1;

				} 

				// skipping past the ip address since it was processed
				i++;

			}

		} else if (strcmp(argv[i], "--domain") == 0 ) { // processing domain name

			if (i + 1 < argc) {

				if (ip_specified) {
					fprintf(stderr, "Error: cannot specify both ip and domain, either one or the other\n");
					return -1;
				}

				domain_specified = true;

				// setting the ip address with DNS

				struct addrinfo info = {0}, *res;
				info.ai_family = AF_INET;
				info.ai_socktype = SOCK_STREAM;

				if (getaddrinfo(argv[i+1], NULL, &info, &res) != 0) {
					fprintf(stderr,"Error: could not find host name with DNS\n");
					return -1;

				}
				memcpy(&settings.server.sin_addr,  
						&((struct sockaddr_in *) res->ai_addr) ->sin_addr, sizeof(struct in_addr));
				
				freeaddrinfo(res);
				
				// skip past the DNS address since it was processed
				i++;
			}

		} else if (strcmp(argv[i], "--quiet") == 0 ) {

			// suppress alerts and mention highlighting
			settings.quiet = true;

		} else {

			// error: invalid argument
			fprintf(stderr, "Error: invalid argument\n");
			return -1;
		}
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
		perror("Error: could not get username from whoami\n");
		pclose(username);
		return -1;
	}

	// empty username error
	if (settings.username[0] == '\0'){
		perror("Error: username is empty\n");
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
			fprintf(stderr, "Error: nonprintable username\n");
			pclose(username);
			return -1;
		}

	}

    pclose(username);
    username = NULL;
    return 0;
}

ssize_t perform_full_write(int fd, void *buf, size_t n) {
	size_t remaining = n;
	char *curr_position = (char *)buf;

	while (remaining > 0) {

		// attempting full write
		ssize_t bytes_written = write(fd, curr_position, remaining);
		
		if (bytes_written < 0) {
			perror("Error: could not send message\n");
			return -1;
		}

		// moving position to where the short write stopped
		curr_position += bytes_written;
		remaining -= bytes_written;

	}

	return n;
}


ssize_t perform_full_read(int fd, void *buf, size_t n) {
	size_t remaining = n;
	char *curr_position = (char *) buf;

	while (remaining > 0) {
		
		// attempting full read
		ssize_t bytes_read = read(fd, curr_position, remaining);

		if (bytes_read == 0) {
			fprintf(stderr, "server closed connection\n");
			return 0;
		}

		if (bytes_read < 0) {
			perror("Error: could not read incoming message\n");
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

    while(settings.running){
 
        // read message from the server (ensure no short reads)
	if (perform_full_read(socket, &inbound_message, sizeof(inbound_message)) <= 0) {
		settings.running = false;
		break;
	}

	// converting integer types into client byte order
	inbound_message.message_type = ntohl(inbound_message.message_type);
	inbound_message.timestamp = ntohl(inbound_message.timestamp);

	switch(inbound_message.message_type){
		case MESSAGE_RECV: {

			// converting timestamp into a readable date format
			char time_buffer[50];
			time_t time = (time_t) inbound_message.timestamp;
			struct tm structured_time;
			localtime_r(&time, &structured_time);

			// converting into date format and storing it
			strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &structured_time);

			// printing the date and sender username
			fprintf(stdout, "[%s] %s: ", time_buffer, inbound_message.username);

			if (!settings.quiet){

				// searching for mentions and highlighting

				char *mention_position;
				char *current_position = inbound_message.message;
	
				// creating the mention format to search for
				char mention_format[sizeof(settings.username) + 1]; // @ + username + \0

				snprintf(mention_format, sizeof(mention_format), "@%s", settings.username);

				// searching for mentions
				while ((mention_position = strstr(current_position, mention_format)) != NULL){
		
					// print everything before the mention
					fprintf(stdout, "%.*s", (int) (mention_position - current_position), current_position);
				
					// the mention with red highlight and ascii bell
					fprintf(stdout, "\a\033[31m%s\033[0m", mention_format);

					current_position = mention_position + strlen(mention_format);
				}

				// printing the rest of the message
				fprintf(stdout, "%s\n", current_position);
			} else {
				fprintf(stdout, "%s\n", inbound_message.message);
			}
			break;
				   }

		case SYSTEM:{
			fprintf(stdout, "\033[90m[SYSTEM] %s\033[0m\n", inbound_message.message);
			break;
			    }

		case DISCONNECT: {

			fprintf(stdout, "\033[31m[DISCONNECT] %s\033[0m\n", inbound_message.message);
			
			settings.disconnected = true;

			// End connection
			settings.running = false;
			return 0;

				 }

		default: {
			
			// unknown message types
			fprintf(stderr, "Error: unknown message type %u %d\n", inbound_message.message_type, sizeof(inbound_message));
		 	break;
			 }
		fflush(stdout);
	}
    }
}

// handler for SIGINT/SIGTERM
void sig_int_term_handler(int signum) {
	signal_status = 1;
	return;
}

int main(int argc, char *argv[]) {
    // setup sigactions (ill-advised to use signal for this project, use sigaction with default (0) flags instead)
    
    // initializing sigaction struct
    struct sigaction act;

    // clearing struct fields to disable masking and flags
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    
    // assigning handler to signals

    // SIGINT and SIGTERM handling
    act.sa_handler = sig_int_term_handler;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGTERM, &act, NULL);

    // parse arguments
    int process_args_status = process_args(argc, argv);

    if (process_args_status == -1) {
	    fprintf(stderr, "Error: could not process arguments\n");
	    return -1;
    }

    // get username
    int get_username_status = get_username();

    if (get_username_status == -1) {
	    perror("Error: could not get username\n");
	    return -1;
    }

    // create socket
   int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd == -1) {
	    perror("Error: could not create socket\n");
	    close(sockfd);
	    sockfd = -1;
	    return -1;
    }

    // connect to server

    int connect_status = connect(sockfd, (struct sockaddr *)&settings.server, sizeof(settings.server));
    if (connect_status == -1) {
	    perror("Error: could not connect to address\n");
	    close(sockfd);
	    sockfd = -1;
	    return -1;
    }

    // create and send login message
    message_t login_msg = {0};
    
    // setting the login message type 
    login_msg.message_type = htonl(LOGIN);

    login_msg.username[sizeof(login_msg.username)-1] = '\0'; // null terminating the username manually just to make sure

    // setting the username
    strncpy(login_msg.username, settings.username, sizeof(settings.username)-1);

    // sending the message to server
    int send_login_message = write(sockfd, &login_msg, sizeof(login_msg));

    if (send_login_message < sizeof(login_msg)) {
	    perror("Error: could not send login message\n");
	    close(sockfd);
	    sockfd = -1;
	    return -1;
    }

    // create and start receive messages thread
    pthread_t receive_thread;

    settings.running = true;

    if((pthread_create(&receive_thread, NULL, receive_messages_thread, (void *) (intptr_t) sockfd)) != 0) {
	    perror("Error: could not create receive thread\n");
	    close(sockfd);
	    settings.running = false;
	    return -1;
    }
 
    char *input = NULL;
    size_t len = 0;

    while(settings.running) {

	    // reading input from stdin
	    ssize_t bytes_read = getline(&input, &len, stdin);

	    // logout and cleanup on any signal
	    if (signal_status != 0) {
		settings.running = false;
		break;
	    }


	    // stripping newline character
	    if (bytes_read > 0 && input[bytes_read-1] == '\n') {
		    input[bytes_read-1] = '\0';
		    bytes_read--;
	    }

	    if (bytes_read > 1023) {
		    fprintf(stderr, "Error: Message too long (1 - 1023 chars)\n");
		    continue;
	    }

	    // checking for an empty message
	    
	    if (input[0] == '\0') {
		    fprintf(stderr, "Error: empty message\n");
		    continue;
	    }

	    // checking that the message does not contain non printable ascii
	    int printable = 1;

	    for (int i = 0; i < bytes_read; i++) {
		    if (!isprint(input[i])){
			    printable = 0;
			    break;
		    }
	    }

	    if (printable == 0){
		    fprintf(stderr, "Message cannot contain non-printable ASCII characters\n");
		    continue;
	    }


	    // initialize a struct to hold the message to send
	    message_t send_message = {0};

	    // setting the message type
	    send_message.message_type = htonl(MESSAGE_SEND);

	    // copying the message to the message struct
	    strncpy(send_message.message, input, sizeof(send_message.message)-1);
	    send_message.message[sizeof(send_message.message)-1] = '\0';
	
	    // sending the message to server
	    if(perform_full_write(sockfd, &send_message, sizeof(send_message)) == -1) {
		    perror("Error: could not send message\n");
		    continue;
	    }

    }

    // logging out
    message_t logout = {0};

    logout.message_type = htonl(LOGOUT);

    // sending the logout message
    if (!settings.disconnected) {
   	 write(sockfd, &logout, sizeof(logout));
    }
    
    close(sockfd);
    sockfd = -1;

    // waiting for thread
    pthread_join(receive_thread, NULL);

    // cleaning up
    free(input);
    input = NULL;
    return 0;
}
