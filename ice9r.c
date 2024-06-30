/* ice9r - Client to remotely run a command on a Windows 9x computer
 * Copyright (C) 2024 Daniel Collins <solemnwarning@solemnwarning.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *     2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *
 *     3. Neither the name of the copyright holder nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
*/

#define ICE9_DEFAULT_PORT 5424

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sysexits.h>
#include <unistd.h>

struct MessageHeader
{
	unsigned char command;
	uint16_t payload_length;
} __attribute__((packed));

static void cmdline_push_char(char **cmdline_buf, size_t *cmdline_size, size_t *cmdline_len, char c, size_t repeat);
static void cmdline_push_string(char **cmdline_buf, size_t *cmdline_size, size_t *cmdline_len, const char *arg);
static void print_usage(FILE *output, const char *argv0);

static void cmdline_push_char(char **cmdline_buf, size_t *cmdline_size, size_t *cmdline_len, char c, size_t repeat)
{
	if((*cmdline_len + repeat) > *cmdline_size)
	{
		*cmdline_size = *cmdline_len + repeat + 1025;
		*cmdline_size -= *cmdline_size % 1024;
		
		*cmdline_buf = realloc(*cmdline_buf, *cmdline_size);
		if(*cmdline_buf == NULL)
		{
			fprintf(stderr, "Unable to allocate memory\n");
			exit(EX_SOFTWARE);
		}
	}
	
	for(size_t i = 0; i < repeat; ++i)
	{
		assert(*cmdline_len < *cmdline_size);
		
		(*cmdline_buf)[*cmdline_len] = c;
		++(*cmdline_len);
	}
}

static void cmdline_push_string(char **cmdline_buf, size_t *cmdline_size, size_t *cmdline_len, const char *arg)
{
	size_t arg_len = strlen(arg);
	
	if(*cmdline_len > 0)
	{
		cmdline_push_char(cmdline_buf, cmdline_size, cmdline_len, ' ', 1);
	}
	
	cmdline_push_char(cmdline_buf, cmdline_size, cmdline_len, '"', 1);
	
	for(size_t i = 0; i < arg_len;)
	{
		if(arg[i] == '"')
		{
			cmdline_push_char(cmdline_buf, cmdline_size, cmdline_len, '\\', 1);
			cmdline_push_char(cmdline_buf, cmdline_size, cmdline_len, '"', 1);
			
			++i;
		}
		else if(arg[i] == '\\')
		{
			size_t bscount = strspn((arg + i), "\\");
			i += bscount;
			
			if(arg[i] == '"')
			{
				/* Backslashes followed by a quote, escape each backslash and the
				 * quote to encode a literal quote in the string.
				*/
				
				cmdline_push_char(cmdline_buf, cmdline_size, cmdline_len, '\\', (2 * bscount) + 1);
				cmdline_push_char(cmdline_buf, cmdline_size, cmdline_len, '"', 1);
				
				++i;
			}
			else if(arg[i] == '\0')
			{
				/* Backslashes at the end of the string need to be escaped and
				 * the terminating quote at the end left unescaped.
				*/
				
				cmdline_push_char(cmdline_buf, cmdline_size, cmdline_len, '\\', (2 * bscount));
			}
			else{
				/* Backslashes followed by a non-backslash character aren't parsed
				 * specifically by CommandLineToArgvW() and don't need escaping.
				*/
				
				cmdline_push_char(cmdline_buf, cmdline_size, cmdline_len, '\\', bscount);
			}
		}
		else{
			cmdline_push_char(cmdline_buf, cmdline_size, cmdline_len, arg[i], 1);
			++i;
		}
	}
	
	cmdline_push_char(cmdline_buf, cmdline_size, cmdline_len, '"', 1);
}

static void print_usage(FILE *output, const char *argv0)
{
	fprintf(output, "Usage: %s <IP address> [-p <port>] <executable> [<arguments> ...]\n", argv0);
	fprintf(output, "       %s <IP address> [-p <port>] <executable> [-e <command line>]\n", argv0);
	fprintf(output, "\n");
	fprintf(output, "The first invocation shown above encodes any given arguments into the process\n");
	fprintf(output, "argument string in the \"standard\" Windows style.\n");
	fprintf(output, "\n");
	fprintf(output, "The second above invocation allows providing an exact argument string, for\n");
	fprintf(output, "programs which have non-standard argument parsing rules.\n");
}

static void send_header(int sock, unsigned char command, uint16_t payload_length);
static void send_all(int sock, const void *data, ssize_t length);

static void send_header(int sock, unsigned char command, uint16_t payload_length)
{
	struct MessageHeader header = { command, payload_length };
	send_all(sock, &header, sizeof(header));
}

static void send_all(int sock, const void *data, ssize_t length)
{
	const char *p = (const char*)(data);
	
	while(length > 0)
	{
		ssize_t sent = send(sock, p, length, 0);
		if(sent < 0)
		{
			perror("send");
			exit(EX_IOERR);
		}
		
		p += sent;
		length -= sent;
	}
}

static void stream_output(FILE *output, int sock, size_t length)
{
	static char buf[1024];
	
	while(length > 0)
	{
		int r = recv(sock, buf, length > sizeof(buf) ? sizeof(buf) : length, 0);
		assert(r > 0);
		
		assert(fwrite(buf, r, 1, output) == 1);
		
		length -= r;
	}
}

int main(int argc, char **argv)
{
	bool skip_args = false;
	
	const char *host = NULL;
	int port = ICE9_DEFAULT_PORT;
	
	const char *program_name = NULL;
	const char *verbatim_cmdline = NULL;
	
	char *cmdline_buf = NULL;
	size_t cmdline_size = 0;
	size_t cmdline_len = 0;
	
	size_t num_cmdline_args = 0;
	
	for(int i = 1; i < argc; ++i)
	{
		if(!skip_args && argv[i][0] == '-')
		{
			if(strcmp(argv[i], "-p") == 0)
			{
				++i;
				
				if(i >= argc)
				{
					fprintf(stderr, "Option '-p' requires a parameter\n");
					return EX_USAGE;
				}
				
				port = atoi(argv[i]);
			}
			else if(strcmp(argv[i], "-e") == 0)
			{
				++i;
				
				if(i >= argc)
				{
					fprintf(stderr, "Option '-e' requires a parameter\n");
					return EX_USAGE;
				}
				
				verbatim_cmdline = argv[i];
			}
			else if(strcmp(argv[i], "--") == 0)
			{
				skip_args = true;
			}
			else{
				fprintf(stderr, "Unrecognised option: %s\n", argv[i]);
				return EX_USAGE;
			}
		}
		else if(host == NULL)
		{
			host = argv[i];
		}
		else if(program_name == NULL)
		{
			program_name = argv[i];
			cmdline_push_string(&cmdline_buf, &cmdline_size, &cmdline_len, argv[i]);
		}
		else{
			cmdline_push_string(&cmdline_buf, &cmdline_size, &cmdline_len, argv[i]);
			++num_cmdline_args;
		}
	}
	
	if(program_name == NULL)
	{
		print_usage(stderr, argv[0]);
		return EX_USAGE;
	}
	
	if(strlen(program_name) > 65535)
	{
		fprintf(stderr, "Program name too long\n");
		return EX_DATAERR;
	}
	
	const char *cmdline = NULL;
	
	if(verbatim_cmdline != NULL)
	{
		if(num_cmdline_args > 0)
		{
			fprintf(stderr, "Additional command line arguments cannot be specified when using -e option\n");
			return EX_USAGE;
		}
		
		cmdline = verbatim_cmdline;
		cmdline_len = strlen(cmdline);
	}
	else{
		cmdline = cmdline_buf;
	}
	
	if(cmdline_len > 65535)
	{
		fprintf(stderr, "Command line arguments are too long\n");
		return EX_DATAERR;
	}
	
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	assert(sock >= 0);
	
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(host);
	addr.sin_port = htons(port);
	
	assert(connect(sock, (struct sockaddr*)(&addr), sizeof(addr)) == 0);
	
	send_header(sock, 'A', strlen(program_name));
	send_all(sock, program_name, strlen(program_name));
	
	send_header(sock, 'C', cmdline_len);
	send_all(sock, cmdline, cmdline_len);
	
	send_header(sock, 'E', 0);
	
	int stdin_fd = fileno(stdin);
	
	while(1)
	{
		fd_set read_fds;
		FD_ZERO(&read_fds);
		
		FD_SET(sock, &read_fds);
		int maxfd = sock;
		
		if(stdin_fd >= 0)
		{
			FD_SET(stdin_fd, &read_fds);
			
			if(stdin_fd > sock)
			{
				maxfd = stdin_fd;
			}
		}
		
		select((maxfd + 1), &read_fds, NULL, NULL, NULL);
		
		if(FD_ISSET(sock, &read_fds))
		{
			struct MessageHeader header;
			
			int r = recv(sock, &header, sizeof(header), 0);
			assert(r == sizeof(header));
			
			switch(header.command)
			{
				case 'O':
					if(header.payload_length == 0)
					{
						fclose(stdout);
						stdout = NULL;
					}
					else if(stdout != NULL)
					{
						stream_output(stdout, sock, header.payload_length);
					}
					
					break;
					
				case 'E':
					if(header.payload_length == 0)
					{
						fclose(stderr);
						stderr = NULL;
					}
					else if(stderr != NULL)
					{
						stream_output(stderr, sock, header.payload_length);
					}
					
					break;
					
				case 'X':
					int32_t exit_code;
					
					assert(header.payload_length == sizeof(exit_code));
					assert(recv(sock, &exit_code, sizeof(exit_code), 0) == sizeof(exit_code));
					
					close(sock);
					
					return exit_code;
			}
		}
		
		if(stdin_fd >= 0 && FD_ISSET(stdin_fd, &read_fds))
		{
			static char buf[1024];
			
			int r = read(stdin_fd, buf, sizeof(buf));
			assert(r >= 0);
			
			send_header(sock, 'I', r);
			send_all(sock, buf, r);
			
			if(r == 0)
			{
				/* End of file. */
				stdin_fd = -1;
			}
		}
	}
	
	close(sock);
	free(cmdline_buf);
	
	return 1;
}
