/*******************************************************************************
#                                                                              #
#      Copyright (C) 2011 Eugene Katsevman                                     #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>


#include "version.h"

#include "mjpg-proxy.h"

#include "misc.h"



#define HEADER 1
#define CONTENT 0
#define NETBUFFER_SIZE 1024 * 4
#define TRUE 1
#define FALSE 0

const char * CONTENT_LENGTH = "Content-Length: ";
// TODO: this must be decoupled from mjpeg-streamer
const char * BOUNDARY =     "--boundarydonotcross";

void init_extractor_state(struct extractor_state * state) {
    state->length = 0;
    state->part = HEADER;
    state->last_four_bytes = 0;
    state->header_contentlength = 0;
    state->contentlength.string = CONTENT_LENGTH;
    state->boundary.string = BOUNDARY;
    search_pattern_reset(&state->contentlength);
    search_pattern_reset(&state->boundary);
}

void init_mjpg_proxy(struct extractor_state * state){
    state->hostname = strdup("localhost");
    state->port = strdup("8080");
    state->uri = strdup("/?action=stream");

    init_extractor_state(state);
}

// main method
// we process all incoming buffer byte per byte and extract binary data from it to state->buffer
// if boundary is detected, then callback for image processing is run
// TODO; decouple from mjpeg streamer and ensure content-length processing
//       for that, we must properly work with headers, not just detect them
void extract_data(struct extractor_state * state, char * buffer, int length) {
    int i;
    DBG("extract_data, len=%d\n", length);
    for (i = 0; i < length && !*(state->should_stop); i++) {
        switch (state->part) {
        case HEADER:
            push_byte(&state->last_four_bytes, buffer[i]);
            if (is_crlfcrlf(state->last_four_bytes)) {
		buffer[i]='\0';
		DBG("end of header:\n%s\n", buffer);
                state->part = CONTENT;
	    }
            else if (is_crlf(state->last_four_bytes))
                search_pattern_reset(&state->contentlength);
            else {
                search_pattern_compare(&state->contentlength, buffer[i]);
                if (search_pattern_matches(&state->contentlength)) {
		    char lenbuf[16];
		    strncpy(lenbuf, buffer+i, min(length-i,sizeof(lenbuf)));
		    lenbuf[sizeof(lenbuf)-1]='\0';
                    DBG("Content length found: %s\n", lenbuf);
		    state->header_contentlength = atoi(lenbuf);
                    DBG("Content length parsed: %d\n", state->header_contentlength);
                    search_pattern_reset(&state->contentlength);
                }
            }
            break;

        case CONTENT:
            if (state->length >= BUFFER_SIZE - 1) {
                perror("Buffer too small\n");
                break;
            }
            state->buffer[state->length++] = buffer[i];

	    if (state->header_contentlength && (state->length == state->header_contentlength)) {
		// If the header contained a content-length, presume it is a
		// single JPEG (not an indefinite stream)
                DBG("Single image of length %d received\n", (int)state->length);
                if (state->on_image_received) // callback
                  state->on_image_received(state->buffer, state->length);
		return;
	    }

            search_pattern_compare(&state->boundary, buffer[i]);
            if (search_pattern_matches(&state->boundary)) {
                state->length -= (strlen(state->boundary.string)+2); // magic happens here
                DBG("Image of length %d received\n", (int)state->length);
                if (state->on_image_received) // callback
                  state->on_image_received(state->buffer, state->length);
                init_extractor_state(state); // reset fsm
            }
            break;
        }

    }

}

//char request [] = "GET /?action=stream HTTP/1.0\r\n\r\n";
char request [1024] = "GET /?action=stream HTTP/1.0\r\nAccept: */*\r\n\r\n";
char request_format [] = "GET %s HTTP/1.0\r\nAccept: */*\r\n\r\n";

void send_request_and_process_response(struct extractor_state * state) {
    int recv_length;
    char netbuffer[NETBUFFER_SIZE];

    init_extractor_state(state);
    
    // send request
    DBG("SEND: %s", request);
    send(state->sockfd, request, sizeof(request), 0);

    // and listen for answer until sockerror or THEY stop us 
    // TODO: we must handle EINTR here, it really might occur
    while ( (recv_length = recv(state->sockfd, netbuffer, sizeof(netbuffer), 0)) > 0 && !*(state->should_stop))
        extract_data(state, netbuffer, recv_length);

}

// TODO:this must be reworked to decouple from mjpeg-streamer
void show_help(char * program_name) {

fprintf(stderr, " ---------------------------------------------------------------\n" \
                " Help for input plugin..: %s\n" \
                " ---------------------------------------------------------------\n" \
                " The following parameters can be passed to this plugin:\n\n" \
                " [-v | --version ]........: current SVN Revision\n" \
                " [-h | --help]............: show this message\n"
                " [-H | --host]............: select host to data from, localhost is default\n"
                " [-p | --port]............: port, defaults to 8080\n"
                " [-i | --interval]........: frame interval in ms, defaults to 1000ms\n"
                " [-p | --uri].............: uri, defaults to '/?action=stream'\n"
                " ---------------------------------------------------------------\n", program_name);
}
// TODO: this must be reworked, too. I don't know how
void show_version() {
    printf("Version - %s\n", VERSION);
}

int parse_cmd_line(struct extractor_state * state, int argc, char * argv []) {
    while (TRUE) {
        static struct option long_options [] = {
            {"help", no_argument, 0, 'h'},
            {"version", no_argument, 0, 'v'},
            {"host", required_argument, 0, 'H'},
            {"port", required_argument, 0, 'p'},
            {"uri", required_argument, 0, 'u'},
            {"interval", required_argument, 0, 'i'},
            {0,0,0,0}
        };

        int index = 0, c = 0;
        c = getopt_long_only(argc,argv, "hvH:p:u:", long_options, &index);

        if (c==-1) break;

        if (c=='?'){
            show_help(argv[0]);
            return 1;
            }
        else
            switch (c) {
            case 'h' :
                show_help(argv[0]);
                return 1;
                break;
            case 'v' :
                show_version();
                return 1;
                break;
            case 'H' :
                free(state->hostname);
                state->hostname = strdup(optarg);
                break;
            case 'p' :
                free(state->port);
                state->port = strdup(optarg);
                break;
            case 'i' :
                state->interval = atoi(optarg);
                break;
            case 'u' :
                free(state->uri);
                state->uri = strdup(optarg);
                break;
            }
    }

  snprintf(request, sizeof(request), request_format, state->uri);
  DBG("Request is %s\n", request);
  
  return 0;
}

// TODO: consider using hints for http

// TODO: consider moving delays to plugin command line arguments
void connect_and_stream(struct extractor_state * state){
    struct addrinfo * info, * rp;
    int errorcode;
    while (TRUE) {
        errorcode = getaddrinfo(state->hostname, state->port, NULL, &info);
        if (errorcode) {
            perror(gai_strerror(errorcode));
        };
        for (rp = info ; rp != NULL; rp = rp->ai_next) {
            state->sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (state->sockfd <0) {
                perror("Can't allocate socket, will continue probing\n");
                continue;
            }

            DBG("socket value is %d\n", state->sockfd);
            if (connect(state->sockfd, (struct sockaddr *) rp->ai_addr, rp->ai_addrlen)>=0 ) {
                DBG("connected to host\n");
                break;
            }

            close(state->sockfd);

        }

        freeaddrinfo(info);

        if (rp==NULL) {
            perror("Can't connect to server, will retry in 5 sec");
            sleep(5);
        }
        else
        {
            send_request_and_process_response(state);
            
            DBG ("Closing socket\n");
            close (state->sockfd);
            if (*state->should_stop)
              break;
	    if (state->interval > 0) {
		usleep(state->interval*1000);
	    }
        };
    }

}

void close_mjpg_proxy(struct extractor_state * state){
    free(state->hostname);
    free(state->port);
}

