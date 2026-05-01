#define _GNU_SOURCE
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
#include <termios.h>
#include <sys/ioctl.h>

#define MAX_MESSAGES 500
#define MAX_MSG_LEN 1100

typedef struct {
	char lines[MAX_MESSAGES][MAX_MSG_LEN];
	int msg_count; // total messages stored
	int scroll_offset; // number of lines scrolled up from the bottom
} message_buffer_t;

typedef struct{
	char buf[1024]; // the current input
	int cursor; // the cursor position in buf
} input_bar_t;


void get_terminal_size(int *rows, int *cols){
	// getting the terminal size
	struct winsize dimensions;

	ioctl(STDOUT_FILENO, TIOCGWINSZ, &dimensions);
	*rows = dimensions.ws_row;
	*cols = dimensions.ws_col;
}


// wrapping ANSI sequences into readable function names

void clear_screen(void) {
	printf("\033[2J");
}

void cursor_home(void){
	printf("\033[H");
}

void cursor_to_row_col(int row, int col){
	printf("\033[%d;%dH", row, col);
}

void cursor_save(void){
	printf("\033[s");
}

void cursor_restore(void){
	printf("\033[u");
}

void erase_line(void){
	printf("\033[K");
}

void cursor_hide(void){
	printf("\033[?25l");
}

void cursor_show(void){
	printf("\033[?25h");
}

static struct termios orig_termios;

void enable_raw_mode(void) {
	tcgetattr(STDIN_FILENO, &orig_termios);

	struct termios raw = orig_termios;

	// disable canonical mode and echo
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void disable_raw_mode(void) {
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}


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
    bool tui;
    char username[32];
} settings_t;

static message_buffer_t mb = {0};
static input_bar_t current_ib = {0};
static pthread_mutex_t mb_lock = PTHREAD_MUTEX_INITIALIZER;

static char* COLOR_RED = "\033[31m";
static char* COLOR_GRAY = "\033[90m";
static char* COLOR_RESET = "\033[0m";
static settings_t settings = {0};

volatile sig_atomic_t signal_status = 0;

void redraw() {
	struct winsize w;
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
	int rows = w.ws_row;
	int msg_rows = rows-1;

	write(STDOUT_FILENO, "\033[?25l", 6); // hide cursor during redraw

	// draw messages from the top
	int start = mb.msg_count - msg_rows - mb.scroll_offset;
	if (start < 0) start = 0;

	for (int row = 1; row <= msg_rows; row++) {

		// move to row then clear it
		cursor_to_row_col(row, 1);
		erase_line();

		int idx = start + (row-1);
		if (idx < mb.msg_count){
			printf("%s", mb.lines[idx]);
		}
	}

	// draw input bar at bottom
	cursor_to_row_col(rows, 1);
	printf("\033[K> %s", current_ib.buf);

	// show cursor
	write(STDOUT_FILENO, "\033[?25h", 6);
	fflush(stdout);
}
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

int process_args(int argc, char *argv[]) {

	bool ip_specified = false;
	bool domain_specified = false;

	settings.server.sin_family = AF_INET;
	settings.server.sin_port = htons(8080);
	inet_pton(AF_INET, "127.0.0.1", &settings.server.sin_addr);

	// parsing arguments
	for (int i = 1; i < argc; i++) {
		
		if (strcmp(argv[i], "--help") == 0) { // processing help argument

			fputs(HELP_MSG, stdout);
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

			} else {
				fprintf(stderr, "Error: missing IP address\n");
				return -1;
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

		} else if (strcmp(argv[i], "--tui") == 0){
			settings.tui = true;
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
			perror("Error: could not send message");
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
	    char formatted[MAX_MSG_LEN] = {0};
	    int curr_position = 0; // variable storing position of last copied character in formatted string buffer
 
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
			curr_position += snprintf(formatted+curr_position, MAX_MSG_LEN-curr_position, "[%s] %s: ", time_buffer, inbound_message.username);

			bool mentioned = false;

			if (!settings.quiet){

				// searching for mentions and highlighting

				char *mention_position;
				char *current_position = inbound_message.message;
	
				// creating the mention format to search for
				char mention_format[sizeof(settings.username) + 1]; // @ + username + \0

				snprintf(mention_format, sizeof(mention_format), "@%s", settings.username);

				// searching for mentions
				while ((mention_position = strstr(current_position, mention_format)) != NULL){
		
					// append everything before the mention
					curr_position += snprintf(formatted + curr_position, MAX_MSG_LEN - curr_position, "%.*s", (int) (mention_position - current_position), current_position);
				
					// append the mention with red highlight and ascii bell
					curr_position += snprintf(formatted + curr_position, MAX_MSG_LEN - curr_position, "%s%s%s", COLOR_RED, mention_format, COLOR_RESET);

					current_position = mention_position + strlen(mention_format);
					
					mentioned = true;
				}

				// appending the rest of the message
				curr_position += snprintf(formatted + curr_position, MAX_MSG_LEN - curr_position, "%s\n", current_position);
			} else {
				curr_position += snprintf(formatted + curr_position, MAX_MSG_LEN - curr_position, "%s\n", inbound_message.message);
			}

			// the bell is written to stdout so that the redraw function does not trigger it every time redraw is called
			if (mentioned) {
				write(STDOUT_FILENO, "\a", 1);
			}

			if (settings.tui) {
				pthread_mutex_lock(&mb_lock);
				strncpy(mb.lines[mb.msg_count % MAX_MESSAGES], formatted, MAX_MSG_LEN - 1);
				mb.msg_count++;
				redraw();
				pthread_mutex_unlock(&mb_lock);
			} else {
				fprintf(stdout, "%s", formatted);
			}
			break;
		}

		case SYSTEM:{
			snprintf(formatted, MAX_MSG_LEN, "%s[SYSTEM] %s%s\n", COLOR_GRAY, inbound_message.message, COLOR_RESET);
			
			if(settings.tui) {
				strncpy(mb.lines[mb.msg_count % MAX_MESSAGES], formatted, MAX_MSG_LEN -1);
				mb.msg_count++;
				redraw();
				pthread_mutex_unlock(&mb_lock);
			} else {
				fprintf(stdout, "%s", formatted);
			}
			
			break;
		}

		case DISCONNECT: {

			snprintf(formatted, MAX_MSG_LEN, "%s[DISCONNECT] %s%s\n", COLOR_RED, inbound_message.message, COLOR_RESET);
			
			if(settings.tui) {
				pthread_mutex_lock(&mb_lock);
				strncpy(mb.lines[mb.msg_count & MAX_MESSAGES], formatted, MAX_MSG_LEN -1);
				mb.msg_count++;
				redraw();
				pthread_mutex_unlock(&mb_lock);
			} else {
				fprintf(stdout, "%s", formatted);
			}

			settings.disconnected = true;

			// End connection
			settings.running = false;
			return 0;

				 }

		default: {
			
			// unknown message types
			fprintf(stderr, "Error: unknown message type %u %zu\n", inbound_message.message_type, sizeof(inbound_message));
		 	break;
			 }
	}
    }
    return NULL;
}

// handler for SIGINT/SIGTERM
void sig_int_term_handler(int signum) {
	signal_status = 1;
	return;
}

void send_chat_message(int sockfd, char *input, ssize_t len){
	    
	    // stripping newline character
	    if (len > 0 && input[len-1] == '\n') {
		    input[len-1] = '\0';
		    len--;
	    }

	    // checking for an empty message
	    if (len > 1023) {
		    fprintf(stderr, "Error: message too long (1-1024 characters\n");
		    return;
	    }

	    if (input[0] == '\0') {
		    fprintf(stderr, "Error: empty message\n");
		    return;
	    }

	    // checking that the message does not contain non printable ascii
	    int printable = 1;

	    for (int i = 0; i < len; i++) {
		    if (!isprint(input[i])){
			    printable = 0;
			    break;
		    }
	    }

	    if (printable == 0){
		    fprintf(stderr, "Error: Message cannot contain non-printable ASCII characters\n");
		    return;
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
	    
	    }

}

void tui_input_loop(int sockfd) {

	enable_raw_mode();

	while (settings.running){
		char c;
		if(read(STDIN_FILENO, &c, 1) <= 0) break;

		if (c == '\033'){
			char seq[2];
			read(STDIN_FILENO, &seq[0], 1);
			read(STDIN_FILENO, &seq[1], 1);
			if (seq[0] == '[') {

				struct winsize w;
				ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
				int msg_rows = w.ws_row - 1;

				if (seq[1] == 'A') mb.scroll_offset++; // move up
				if (seq[1] == 'B') mb.scroll_offset--; // move down

				if (mb.scroll_offset < 0) mb.scroll_offset = 0; // can't scroll past bottom

				// can't scroll past the top
				int max_scroll = mb.msg_count - msg_rows;
				if (max_scroll < 0) max_scroll = 0;
				if (mb.scroll_offset > max_scroll) mb.scroll_offset = max_scroll;
			}
		} else if (c=='\n' || c == '\r') {
			//submit message
			if (current_ib.cursor > 0) {
				current_ib.buf[current_ib.cursor] = '\0';

				//validate and send
				send_chat_message(sockfd, current_ib.buf, current_ib.cursor);

				//clear input
				memset(current_ib.buf, 0, sizeof(current_ib.buf));
				current_ib.cursor = 0;
				mb.scroll_offset = 0; // move back to the bottom
			}
		} else if (c == 127 || c == '\b') {
			if (current_ib.cursor >0){
				current_ib.buf [--current_ib.cursor] = '\0';
			}
		} else if (c == 4) {
			break;
		} else if (isprint(c) && current_ib.cursor < 1023) {
			current_ib.buf[current_ib.cursor++] = c;
		}
		
		redraw();
			
	}	

	disable_raw_mode();
}


void tui_cleanup(void) {
	disable_raw_mode();
	clear_screen();
	cursor_home();
	cursor_show();
}
int main(int argc, char *argv[]) {


    // immediate outputs
    setbuf(stdout, NULL);

    // setup sigactions (ill-advised to use signal for this project, use sigaction with default (0) flags instead)
    
    // initializing sigaction struct
    struct sigaction act = {0};

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

    if (connect(sockfd, (struct sockaddr *) &settings.server, sizeof(settings.server)) == -1) {
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

    if (perform_full_write(sockfd, &login_msg, sizeof(login_msg)) == -1) {
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

    if (settings.tui) {
	    tui_input_loop(sockfd);
    } else{

   	 while(settings.running) {
	   	  
	 	  // reading input from stdin
	  	  ssize_t bytes_read = getline(&input, &len, stdin);

	  	  // logout and cleanup on any signal
	 	   if (signal_status != 0 || bytes_read == -1) {
			settings.running = false;
			break;
	  	  }

	   	 send_chat_message(sockfd, input, bytes_read);
  	  }
    }

    // logging out
    message_t logout = {0};

    logout.message_type = htonl(LOGOUT);

    // sending the logout message
    if (!settings.disconnected) {
   	 perform_full_write(sockfd, &logout, sizeof(logout));
    }
    
    close(sockfd);
    sockfd = -1;

    // waiting for thread
    pthread_join(receive_thread, NULL);

    // cleaning up
    free(input);
    input = NULL;

    if(settings.tui){
	    tui_cleanup();
    }
    return 0;
}
