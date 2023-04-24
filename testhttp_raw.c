#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include "err.h"

#define BUFFER_SIZE 2000

char buffer[BUFFER_SIZE + 1];
char last30[30];
int buffer_pos = 0, sock;
int total_length = 0;

void divide(char *targets[], char *arg, int skipHTTP) {
    if(skipHTTP) {
        if(arg[4] == 's')
            arg += 8;
        else
            arg += 7;
    }
    int i = 0;
    int letter = 0;
    targets[0][0] = '\0';
    targets[1][0] = '\0';
    targets[2][0] = '/';
    targets[2][1] = '\0';
    for(char *j = arg; *j != '\0'; j++) {
        if(*j == ':' && i == 0) {
            i = 1;
            letter = 0;
        }
        else if(*j == '/' && i < 2) {
            i = 2;
            letter = 1;
        }
        else
            targets[i][letter++] = *j;
        targets[i][letter] = '\0';
    }
}

void prepareToWrite(int count, ...) {
    va_list strings;
    va_start(strings, count);

    for(int i = 0; i < count; i++) {
        char *string = va_arg(strings, char*);
        int remaining = strlen(string);
        int written = 0;

        while(remaining > 0) {
            strncpy(buffer + buffer_pos, string + written, BUFFER_SIZE - buffer_pos);
            if(remaining <= BUFFER_SIZE - buffer_pos) {
                buffer_pos += remaining;
                remaining = 0;
            }
            else {
                if(write(sock, buffer, sizeof(buffer) - 1) == -1)
                    syserr("write");
                remaining -= BUFFER_SIZE - buffer_pos;
                written += BUFFER_SIZE - buffer_pos;
                buffer_pos = 0;
                memset(buffer, 0, sizeof(buffer));
            }
        }
    }

    va_end(strings);
}

int isOWS(char c) {
    return c == ' ' || c == '\t';
}

int isWhitespace(char c) {
    return isOWS(c) || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

int hexToNum(char c) {
    if(c >= '0' && c <= '9')
        return c - '0';

    if(c >= 'A' && c <= 'Z')
        c += 32;

    return 10 + c - 'a';
}

int searchString(char c) {
    static char *strings_to_search[] = {"HTTP/1.1 200 OK\r\n", "\r\n\r\n", "\r\n", "transfer-encoding: chunked", "set-cookie:"};

    for(int i = 0; i < 29; i++)
        last30[i] = last30[i + 1];
    last30[29] = c;

    for(int i = 0; i < 5; i++)
        if(strncmp(last30 + 30 - strlen(strings_to_search[i]), strings_to_search[i], strlen(strings_to_search[i])) == 0)
            return i;

    return -1;
}

void printCookies() {
    int expect_name = 1;
    int get_value = 0;
    int is_header = 1;
    int is_chunked = 0;
    int chunk_left = 0;
    int len;
    int get_chunk_size = 0;
    int first_line_letter = 0;
    char first_line[2001];
    memset(first_line, 0, sizeof(first_line));
    while(1) {
        memset(buffer, 0, sizeof(buffer));
        len = read(sock, buffer, sizeof(buffer) - 1);

        if(len <= 0)
            break;

        for(int i = 0; i < len; i++) {
            if(first_line_letter == -1 && is_header && !get_value && buffer[i] >= 'A' && buffer[i] <= 'Z')
                buffer[i] += 32;

            int search = searchString(buffer[i]);

            if(first_line_letter != -1) {
                int pos = first_line_letter % 2000;
                if(pos == 1999) {
                    int offset = 0;
                    if (strncmp(first_line, "HTTP/1.1", 8) == 0)
                        offset = 9;
                    
                    printf("%s", first_line + offset);
                    memset(first_line, 0, sizeof(first_line));
                }
                
                first_line[pos] = buffer[i];
                first_line_letter++;

                if(search == 2 || (search == 0 && first_line_letter != 17)) {
                    int offset = 0;
                    if (strncmp(first_line, "HTTP/1.1", 8) == 0)
                        offset = 9;

                    printf("%s", first_line + offset);
                    close(sock);
                    exit(0);
                }
                else if(search == 0) {
                    first_line_letter = -1;
                    search = 2;
                }
            }

            if(!is_header && is_chunked) {
                if(!get_chunk_size && chunk_left == 0) {
                    chunk_left = -1;
                    get_chunk_size = 1;
                }
                else if(!get_chunk_size)
                    chunk_left--;
                
                if(get_chunk_size) {
                    if(chunk_left != -1 && buffer[i] == '\r') {
                        i++;
                        get_chunk_size = 0;
                        total_length += chunk_left;
                    }
                    else if(!isWhitespace(buffer[i])) {
                        if(chunk_left == -1)
                            chunk_left = 0;

                        chunk_left *= 16;
                        chunk_left += hexToNum(buffer[i]);
                    }

                    continue;
                }
            }
            else if(!is_header)
                total_length++;

            if(is_header && search == 1)
                is_header = 0;
                
            if(search == 2)
                expect_name = 1;

            if(is_header) {
                if(expect_name && search == 3)
                    is_chunked = 1;

                if(get_value == 1) {
                    if(isOWS(buffer[i]))
                        continue;
                    
                    if(buffer[i] == ';' || search == 2) {
                        printf("\n");
                        get_value = 0;
                        expect_name = 1;
                        continue;
                    }

                    printf("%c", buffer[i]);
                }
                else if(expect_name && search == 4) {
                    get_value = 1;
                    expect_name = 0;
                }
            }
        }
    }
}

void writeCookies(char *filename) {
    FILE *cookies_file = fopen(filename, "r");
    int c;
    int write_cookie = 0;
    
    while((c = fgetc(cookies_file)) != EOF) {
        if(write_cookie == 0) {
            prepareToWrite(1, "Set-Cookie: ");
            write_cookie = 1;
        }

        if(c == '\n') {
            prepareToWrite(1, "\r\n");
            write_cookie = 0;
        }
        else
            prepareToWrite(1, &c);
    }

    if(write_cookie == 1)
        prepareToWrite(1, "\r\n");

    fclose(cookies_file);
}

void writeAll() {
    if(write(sock, buffer, strlen(buffer)) == -1)
        syserr("write");
    buffer_pos = 0;
    memset(buffer, 0, sizeof(buffer));
}

int main(int argc, char *argv[]) {
    int rc;
    struct addrinfo addr_hints, *addr_result;

    if(argc != 4)
        fatal("Usage: %s <adres połączenia>:<port> <plik ciasteczek> <testowany adres http>", argv[0]);

    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sock < 0)
        syserr("socket");

    memset(&addr_hints, 0, sizeof(struct addrinfo));
    addr_hints.ai_flags = 0;
    addr_hints.ai_family = AF_INET;
    addr_hints.ai_socktype = SOCK_STREAM;
    addr_hints.ai_protocol = IPPROTO_TCP;

    char *connect_info[3];
    for(int i = 0; i < 3; i++) {
        connect_info[i] = malloc(strlen(argv[1]) * sizeof(char));
        if(connect_info[i] == NULL)
            syserr("malloc");
    }

    char *info[3];
    for(int i = 0; i < 3; i++) {
        info[i] = malloc(strlen(argv[3]) * sizeof(char));
        if(info[i] == NULL)
            syserr("malloc");
    }

    info[2][0] = '/';
    divide(connect_info, argv[1], 0);
    divide(info, argv[3], 1);
    
    rc = getaddrinfo(connect_info[0], connect_info[1], &addr_hints, &addr_result);
    if(rc != 0)
        syserr("getaddrinfo: %s", gai_strerror(rc));

    if(connect(sock, addr_result->ai_addr, addr_result->ai_addrlen) != 0)
        syserr("connect");
    freeaddrinfo(addr_result);

    memset(buffer, 0, sizeof(buffer));
    prepareToWrite(3, "GET ", info[2], " HTTP/1.1\r\n");

    if(info[1][0] != '\0')
        prepareToWrite(5, "Host: ", info[0], ":", info[1], "\r\n");
    else
        prepareToWrite(3, "Host: ", info[0], "\r\n");
    prepareToWrite(1, "Connection: close\r\n");
    writeCookies(argv[2]);
    prepareToWrite(1, "\r\n");
    writeAll();

    printCookies();
    printf("Dlugosc zasobu: %d\n", total_length);

    close(sock);

    for(int i = 0; i < 3; i++) {
        free(connect_info[i]);
        free(info[i]);
    }
}