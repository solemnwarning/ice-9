/* ice9d - Remote command execution server for Windows 9x
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

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <winsock2.h>
#include <windows.h>

#include "pipe9x/pipe9x.h"

#define PORT 5424
#define MAX_CONNECTIONS 16

#define PIPE_READ_SIZE 32768

#define RECVBUF_SIZE (72 * 1024)
#define SENDBUF_SIZE (128 * 1024)

/* Client to server messages:
 *
 * A - Set application_path
 * C - Set command_line
 * W - Set working_directory
 * E - Execute process
 * I - Write bytes to stdin
 *
 * Server to client messages:
 *
 * O - Data read from stdout
 * E - Data read from stderr
 * X - Exit status (followed by close)
*/

enum ConnectionState
{
	CS_SETUP,
	CS_RUNNING,
	CS_CLOSING,
};

struct Connection
{
	int id;
	enum ConnectionState state;
	
	int sock;
	
	unsigned char recvbuf[RECVBUF_SIZE];
	int recvbuf_used;
	
	unsigned char sendbuf[SENDBUF_SIZE];
	int sendbuf_used;
	
	char *application_path;
	char *command_line;
	char *working_directory;
	
	HANDLE process;
	PipeWriteHandle stdin_pipe;
	PipeReadHandle stdout_pipe;
	PipeReadHandle stderr_pipe;
};

struct MessageHeader
{
	unsigned char command;
	uint16_t payload_length;
} __attribute__((packed));

static int next_connection_id = 1;
static struct Connection connections[MAX_CONNECTIONS];
static size_t num_connections = 0;

static void connection_init(int newsock);
static bool store_string(char **dst, const char *src, size_t length);
static bool connection_read(int connection_idx);
static bool connection_write(int connection_idx, unsigned char cmd, const void *payload, int payload_length);
static bool connection_flush(int connection_idx);
static void connection_close(int connection_idx);
static char *path_search(const char *program_name);
static void pipe_read(int connection_idx, PipeReadHandle *pipe9x_handle, unsigned char command);
static void process_exit(int connection_idx);

static void connection_init(int newsock)
{
	if(num_connections == MAX_CONNECTIONS)
	{
		fprintf(stderr, "Too many open connections, dropping connection\n");
		closesocket(newsock);
		
		return;
	}
	
	struct Connection *connection = &(connections[num_connections++]);
	
	connection->id = next_connection_id++;
	connection->state = CS_SETUP;
	
	connection->sock = newsock;
	
	connection->recvbuf_used = 0;
	connection->sendbuf_used = 0;
	
	connection->application_path  = NULL;
	connection->command_line      = NULL;
	connection->working_directory = NULL;
	
	connection->process     = NULL;
	connection->stdin_pipe  = NULL;
	connection->stdout_pipe = NULL;
	connection->stderr_pipe = NULL;
	
	printf("[%d] New connection established\n", connection->id);
}

static bool connection_read(int connection_idx)
{
	struct Connection *connection = &(connections[connection_idx]);
	
	if(connection->recvbuf_used == RECVBUF_SIZE)
	{
		return true;
	}
	
	int read_bytes = recv(connection->sock, (char*)(connection->recvbuf + connection->recvbuf_used), (RECVBUF_SIZE - connection->recvbuf_used), 0);
	
	if(read_bytes <= 0)
	{
		if(read_bytes == 0)
		{
			fprintf(stderr, "Connection closed (end of file)\n");
		}
		else{
			DWORD error = WSAGetLastError();
			
			if(error == WSAEWOULDBLOCK)
			{
				return true;
			}
			else{
				fprintf(stderr, "Connection read error %u\n", (unsigned)(error));
			}
		}
		
		connection_close(connection_idx);
		return false;
	}
	
	connection->recvbuf_used += read_bytes;
	
	while(connection->recvbuf_used >= sizeof(struct MessageHeader))
	{
		struct MessageHeader *header = (struct MessageHeader*)(connection->recvbuf);
		
		const void *payload = header + 1;
		int payload_length = header->payload_length;
		
		if(connection->recvbuf_used >= (sizeof(struct MessageHeader) + payload_length))
		{
			/* Got a complete message. */
			
			switch(header->command)
			{
				case 'A':
				{
					if(!store_string(&(connection->application_path), (const char*)(payload), header->payload_length))
					{
						connection_close(connection_idx);
						return false;
					}
					
					break;
				}
				
				case 'C':
				{
					if(!store_string(&(connection->command_line), (const char*)(payload), header->payload_length))
					{
						connection_close(connection_idx);
						return false;
					}
					
					break;
				}
				
				case 'W':
				{
					if(!store_string(&(connection->working_directory), (const char*)(payload), header->payload_length))
					{
						connection_close(connection_idx);
						return false;
					}
					
					break;
				}
				
				case 'E':
				{
					PipeReadHandle stdin_read, stdout_read, stderr_read;
					PipeWriteHandle stdin_write, stdout_write, stderr_write;
					
					DWORD pipe_error = pipe9x_create(&stdin_read, PIPE_READ_SIZE, TRUE, &stdin_write, PIPE_READ_SIZE, FALSE);
					if(pipe_error != ERROR_SUCCESS)
					{
						fprintf(stderr, "pipe9x_create: %u\n", (unsigned)(pipe_error));
						
						connection_close(connection_idx);
						return false;
					}
					
					pipe_error = pipe9x_create(&stdout_read, PIPE_READ_SIZE, FALSE, &stdout_write, PIPE_READ_SIZE, TRUE);
					if(pipe_error != ERROR_SUCCESS)
					{
						fprintf(stderr, "pipe9x_create: %u\n", (unsigned)(pipe_error));
						
						pipe9x_write_close(stdin_write);
						pipe9x_read_close(stdin_read);
						
						connection_close(connection_idx);
						return false;
					}
					
					pipe_error = pipe9x_create(&stderr_read, PIPE_READ_SIZE, FALSE, &stderr_write, PIPE_READ_SIZE, TRUE);
					if(pipe_error != ERROR_SUCCESS)
					{
						fprintf(stderr, "pipe9x_create: %u\n", (unsigned)(pipe_error));
						
						pipe9x_write_close(stdout_write);
						pipe9x_read_close(stdout_read);

						pipe9x_write_close(stdin_write);
						pipe9x_read_close(stdin_read);
						
						connection_close(connection_idx);
						return false;
					}
					
					STARTUPINFO si;
					memset(&si, 0, sizeof(si));
					
					si.cb         = sizeof(si);
					si.dwFlags    = STARTF_USESTDHANDLES;
					si.hStdInput  = pipe9x_read_pipe(stdin_read);
					si.hStdOutput = pipe9x_write_pipe(stdout_write);
					si.hStdError  = pipe9x_write_pipe(stderr_write);
					
					PROCESS_INFORMATION pi;
					
					fprintf(stderr, "application_path = %s\n", connection->application_path);
					fprintf(stderr, "command_line = %s\n", connection->command_line);
					
					const char *application_path = connection->application_path;
					char *application_path_buf = NULL;
					
					if(
						strchr(application_path, '\\') == NULL
						&& GetFileAttributes(application_path) == INVALID_FILE_ATTRIBUTES)
					{
						/* application_path doesn't contain any slashes and doesn't appear
						 * to exist in the working directory, search PATH for it.
						*/
						
						fprintf(stderr, "[%d] %s not found, searching PATH...\n", connection->id, connection->application_path);
						
						application_path_buf = path_search(connection->application_path);
						if(application_path_buf != NULL)
						{
							fprintf(stderr, "[%d] Found %s\n", connection->id, application_path_buf);
							application_path = application_path_buf;
						}
					}
					
					if(CreateProcess(
						application_path,               /* lpApplicationName */
						connection->command_line,       /* lpCommandLine */
						NULL,                           /* lpProcessAttributes */
						NULL,                           /* lpThreadAttributes */
						TRUE,                           /* bInheritHandles */
						DETACHED_PROCESS,               /* dwCreationFlags */
						NULL,                           /* lpEnvironment */
						connection->working_directory,  /* lpCurrentDirectory */
						&si,                            /* lpStartupInfo */
						&pi))                           /* lpProcessInformation */
					{
						pipe9x_read_close(stdin_read);
						pipe9x_write_close(stdout_write);
						pipe9x_write_close(stderr_write);
						
						CloseHandle(pi.hThread);
						
						connection->process     = pi.hProcess;
						connection->stdin_pipe  = stdin_write;
						connection->stdout_pipe = stdout_read;
						connection->stderr_pipe = stderr_read;
						
						if(pipe9x_read_initiate(stdout_read) != ERROR_IO_PENDING)
						{
							abort();
						}
						
						if(pipe9x_read_initiate(stderr_read) != ERROR_IO_PENDING)
						{
							abort();
						}
					}
					else{
						fprintf(stderr, "CreateProcess: %u\n", (unsigned)(GetLastError()));
						
						free(application_path_buf);
						
						/*
						pipe9x_write_close(stderr_write);
						pipe9x_read_close(stderr_read);
						pipe9x_write_close(stdout_write);
						pipe9x_read_close(stdout_read);
						pipe9x_write_close(stdin_write);
						pipe9x_read_close(stdin_read);
						*/
						
						connection_close(connection_idx);
						return false;
					}
					
					free(application_path_buf);
					
					break;
				}
				
				case 'I':
				{
					if(header->payload_length == 0)
					{
						pipe9x_write_close(connection->stdin_pipe);
						connection->stdin_pipe = NULL;
					}
					else if(connection->stdin_pipe != NULL)
					{
						if(pipe9x_write_pending(connection->stdin_pipe))
						{
							/* Stall until current write on stdin pipe completes. */
							return true;
						}
						
						// fprintf(stderr, "[%d] Writing %u bytes to child stdin\n", connection->id, (unsigned)(header->payload_length));
						
						DWORD error = pipe9x_write_initiate(connection->stdin_pipe, payload, header->payload_length);
						if(error != ERROR_IO_PENDING)
						{
							fprintf(stderr, "[%d] Write error %u on child stdin\n", connection->id, (unsigned)(error));
							
							connection_close(connection_idx);
							return false;
						}
					}
					
					break;
				}
				
				default:
				{
					fprintf(stderr, "Received unrecognised command: %c\n", header->command);
					connection_close(connection_idx);
					
					return false;
				}
			}
			
			int total_length = sizeof(struct MessageHeader) + payload_length;
			
			memmove(connection->recvbuf, (connection->recvbuf + total_length), (connection->recvbuf_used - total_length));
			connection->recvbuf_used -= total_length;
		}
		else{
			break;
		}
	}
	
	return true;
}

static bool store_string(char **dst, const char *src, size_t length)
{
	free(*dst);
	*dst = malloc(length + 1);
	
	if(*dst == NULL)
	{
		fprintf(stderr, "Memory allocation failed\n");
		return false;
	}
	
	memcpy(*dst, src, length);
	(*dst)[length] = '\0';
	
	return true;
}

static bool connection_write(int connection_idx, unsigned char cmd, const void *payload, int payload_length)
{
	assert(payload_length <= 65535);
	
	struct Connection *connection = &(connections[connection_idx]);
	
	int sendbuf_available = SENDBUF_SIZE - connection->sendbuf_used;
	
	if(sendbuf_available < (sizeof(struct MessageHeader) + payload_length))
	{
		connection_close(connection_idx);
		return false;
	}
	
	struct MessageHeader *header = (struct MessageHeader*)(connection->sendbuf + connection->sendbuf_used);
	
	header->command = cmd;
	header->payload_length = payload_length;
	
	memcpy((header + 1), payload, payload_length);
	
	connection->sendbuf_used += sizeof(struct MessageHeader);
	connection->sendbuf_used += payload_length;
	
	return connection_flush(connection_idx);
}

static bool connection_flush(int connection_idx)
{
	struct Connection *connection = &(connections[connection_idx]);
	
	if(connection->sendbuf_used > 0)
	{
		int write_result = send(connection->sock, (const char*)(connection->sendbuf), connection->sendbuf_used, 0);
		if(write_result >= 0)
		{
			memmove(connection->sendbuf, (connection->sendbuf + write_result), (connection->sendbuf_used - write_result));
			connection->sendbuf_used -= write_result;
		}
		else{
			DWORD error = WSAGetLastError();
			
			if(error != WSAEWOULDBLOCK)
			{
				fprintf(stderr, "Connection write error %u\n", (unsigned)(error));
				
				connection_close(connection_idx);
				return false;
			}
		}
	}
	
	if(connection->sendbuf_used == 0 && connection->state == CS_CLOSING)
	{
		connection_close(connection_idx);
		return false;
	}
	
	return true;
}

static void connection_close(int connection_idx)
{
	struct Connection *connection = &(connections[connection_idx]);
	
	closesocket(connection->sock);
	connection->sock = INVALID_SOCKET;
	
	if(connection->process != NULL)
	{
		if(!TerminateProcess(connection->process, -1))
		{
			fprintf(stderr, "TerminateProcess %u\n", (unsigned)(GetLastError()));
		}
		
		CloseHandle(connection->process);
		connection->process = NULL;
	}
	
	/* We should close the pipes here, but due to a bug in Windows 98, the
	 * reads may block forever and make us hang... so we just forget about
	 * them and leave the handles/threads to block forever (#1).
	*/
	
	// pipe9x_write_close(connection->stdin_pipe);
	connection->stdin_pipe = NULL;
	
	// pipe9x_read_close(connection->stderr_pipe);
	connection->stderr_pipe = NULL;
	
	// pipe9x_read_close(connection->stdout_pipe);
	connection->stdout_pipe = NULL;
	
	free(connection->working_directory);
	connection->working_directory = NULL;
	
	free(connection->command_line);
	connection->command_line = NULL;
	
	free(connection->application_path);
	connection->application_path = NULL;
	
	fprintf(stderr, "[%d] Connection closed\n", connection->id);
	
	memmove((connections + connection_idx), (connections + connection_idx + 1), (num_connections - connection_idx - 1));
	--num_connections;
}

static char *path_search(const char *program_name)
{
	const char *PATH = getenv("PATH");
	if(PATH == NULL)
	{
		return NULL;
	}
	
	size_t PATH_LEN = strlen(PATH);
	size_t pn_len = strlen(program_name);
	
	for(size_t i = 0; i < PATH_LEN; ++i)
	{
		size_t elem_len = strcspn((PATH + i), ";");
		
		if(elem_len > 0)
		{
			/* directory '\\' program name ".exe" '\0' */
			char *path_buf = malloc(elem_len + 1 + pn_len + 5);
			if(path_buf != NULL)
			{
				strncpy(path_buf, (PATH + i), elem_len);
				path_buf[elem_len] = '\\';
				path_buf[elem_len + 1] = '\0';
				
				strcat(path_buf, program_name);
				
				if(GetFileAttributes(path_buf) != INVALID_FILE_ATTRIBUTES)
				{
					/* Found it! */
					return path_buf;
				}
				
				strcat(path_buf, ".exe");
				
				if(GetFileAttributes(path_buf) != INVALID_FILE_ATTRIBUTES)
				{
					/* Found it! */
					return path_buf;
				}
				
				free(path_buf);
			}
			
			i += elem_len;
		}
	}
	
	return NULL;
}

static void pipe_read(int connection_idx, PipeReadHandle *pipe9x_handle, unsigned char command)
{
	void *data;
	size_t data_size;
	
	DWORD error = pipe9x_read_result(*pipe9x_handle, &data, &data_size, false);
	
	if(error == ERROR_SUCCESS)
	{
		if(data_size == 0)
		{
			/* Pipes on Windows can propagate zero-sized writes, but this doesn't map
			 * to UNIX pipes, so discard them.
			 *
			 * Read next data from pipe in the background.
			*/
			
			error = pipe9x_read_initiate(*pipe9x_handle);
			if(error == ERROR_BROKEN_PIPE)
			{
				goto HANDLE_EOF;
			}
			else if(error != ERROR_IO_PENDING)
			{
				abort();
			}
			
			return;
		}
		
		// printf("[%d] Read %u bytes from child on %c\n", connections[connection_idx].id, (unsigned)(data_size), command);
	}
	else if(error == ERROR_BROKEN_PIPE)
	{
		HANDLE_EOF:

		/* Write end closed - end of file.
		 * We will send a zero-byte read to the client.
		*/
		
		printf("[%d] Read EOF from child on %c\n", connections[connection_idx].id, command);
		
		pipe9x_read_close(*pipe9x_handle);
		*pipe9x_handle = NULL;
		
		data_size = 0;
	}
	else{
		printf("[%d] Read error %u from child on %c\n", connections[connection_idx].id, (unsigned)(error), command);
		connection_close(connection_idx);
		
		return;
	}
	
	if(connection_write(connection_idx, command, data, data_size) && error == ERROR_SUCCESS)
	{
		/* Read next data from pipe in the background. */
		
		error = pipe9x_read_initiate(*pipe9x_handle);
		if(error == ERROR_BROKEN_PIPE)
		{
			goto HANDLE_EOF;
		}
		else if(error != ERROR_IO_PENDING)
		{
			abort();
		}
	}
}

static void process_exit(int connection_idx)
{
	struct Connection *connection = &(connections[connection_idx]);
	
	DWORD exit_code;
	GetExitCodeProcess(connection->process, &exit_code);
	
	CloseHandle(connection->process);
	connection->process = NULL;
	
	printf("[%d] Process exited with code %u\n", connections[connection_idx].id, (unsigned)(exit_code));
	
	int32_t exit_code_i = exit_code;
	
	connection->state = CS_CLOSING;
	connection_write(connection_idx, 'X', &exit_code_i, sizeof(exit_code_i));
}

int main()
{
	WSADATA wsdata;
	int wserror = WSAStartup(MAKEWORD(2, 0), &wsdata);
	if(wserror != 0)
	{
		fprintf(stderr, "WSAStartup: %d\n", wserror);
		return 1;
	}
	
	//WSAEVENT wsevent = WSACreateEvent();
	WSAEVENT wsevent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if(wsevent == WSA_INVALID_EVENT)
	{
		fprintf(stderr, "WSACreateEvent: %u\n", (unsigned)(WSAGetLastError()));
		return 1;
	}
	
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	if(listener == INVALID_SOCKET)
	{
		fprintf(stderr, "socket: %u\n", (unsigned)(WSAGetLastError()));
		return 1;
	}
	
	struct sockaddr_in bind_addr;
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = htons(PORT);
	
	if(bind(listener, (struct sockaddr*)(&bind_addr), sizeof(bind_addr)) != 0)
	{
		fprintf(stderr, "bind: %u\n", (unsigned)(WSAGetLastError()));
		return 1;
	}
	
	if(listen(listener, 8) != 0)
	{
		fprintf(stderr, "listen: %u\n", (unsigned)(WSAGetLastError()));
		return 1;
	}
	
	WSAEventSelect(listener, wsevent, FD_ACCEPT);
	
	while(TRUE)
	{
		HANDLE wait_handles[1 + MAX_CONNECTIONS * 3];
		size_t num_wait_handles;
		
		wait_handles[0] = wsevent;
		num_wait_handles = 1;
		
		for(size_t i = 0; i < num_connections; ++i)
		{
			int recvbuf_available = RECVBUF_SIZE - connections[i].recvbuf_used;
			int sendbuf_available = SENDBUF_SIZE - connections[i].sendbuf_used;
			
			/* Wait on the stdout/stderr handles only if there is enough space in the
			 * connection's send buffer to queue the maximum potential read size to be
			 * written to the connection.
			*/
			
			if(sendbuf_available >= (sizeof(struct MessageHeader) + PIPE_READ_SIZE))
			{
				if(connections[i].stdout_pipe != NULL)
				{
					wait_handles[num_wait_handles++] = pipe9x_read_event(connections[i].stdout_pipe);
				}
				
				if(connections[i].stderr_pipe != NULL)
				{
					wait_handles[num_wait_handles++] = pipe9x_read_event(connections[i].stderr_pipe);
				}
			}
			
			/* Wait on the process handle only if there is space in the send buffer and
			 * both output pies have been read to end of file.
			*/
			
			if(sendbuf_available >= (sizeof(struct MessageHeader) + sizeof(int32_t)))
			{
				if(connections[i].stdout_pipe == NULL
					&& connections[i].stderr_pipe == NULL
					&& connections[i].process != NULL)
				{
					wait_handles[num_wait_handles++] = connections[i].process;
				}
			}
			
			/* Wait on the stdin handle if there is a write in progress. */
			
			if(connections[i].stdin_pipe != NULL && pipe9x_write_pending(connections[i].stdin_pipe))
			{
				wait_handles[num_wait_handles++] = pipe9x_write_event(connections[i].stdin_pipe);
			}
			
			/* Wait for socket readability if there is space in the receive buffer and
			 * for writability if there is data waiting to be sent.
			*/
			
			long events = 0;
			
			if(recvbuf_available > 0)
			{
				events |= FD_READ;
				events |= FD_CLOSE;
			}
			
			if(connections[i].sendbuf_used > 0)
			{
				events |= FD_WRITE;
			}
			
			WSAEventSelect(connections[i].sock, wsevent, events);
		}
		
		DWORD wait_result = WaitForMultipleObjects(num_wait_handles, wait_handles, FALSE, INFINITE);
		
		if(wait_result == WAIT_FAILED)
		{
			fprintf(stderr, "WaitForMultipleObjects: %u\n", (unsigned)(GetLastError()));
			return 1;
		}
		
		if(wait_result == WAIT_OBJECT_0)
		{
			int newsock = accept(listener, NULL, NULL);
			if(newsock != INVALID_SOCKET)
			{
				connection_init(newsock);
			}
			
			for(int i = 0; i < num_connections;)
			{
				if(connection_flush(i) && connection_read(i))
				{
					++i;
				}
			}
		}
		else{
			assert(wait_result > WAIT_OBJECT_0);
			assert(wait_result < (WAIT_OBJECT_0 + num_wait_handles));
			
			HANDLE woke_handle = wait_handles[wait_result - WAIT_OBJECT_0];
			
			for(size_t i = 0; i < num_connections; ++i)
			{
				if(connections[i].stdout_pipe != NULL
					&& woke_handle == pipe9x_read_event(connections[i].stdout_pipe))
				{
					pipe_read(i, &(connections[i].stdout_pipe), 'O');
					break;
				}
				
				if(connections[i].stderr_pipe != NULL
					&& woke_handle == pipe9x_read_event(connections[i].stderr_pipe))
				{
					pipe_read(i, &(connections[i].stderr_pipe), 'E');
					break;
				}
				
				if(connections[i].stdin_pipe != NULL
					&& woke_handle == pipe9x_write_event(connections[i].stdin_pipe))
				{
					size_t data_written;
					DWORD error = pipe9x_write_result(connections[i].stdin_pipe, &data_written, TRUE);
					
					if(error != ERROR_SUCCESS)
					{
						fprintf(stderr, "[%d] Write error %u on child stdin\n", connections[i].id, (unsigned)(error));
						connection_close(i);
					}
					else{
						// fprintf(stderr, "[%d] Wrote %u bytes to child stdin\n", connections[i].id, (unsigned)(data_written));
					}
					
					break;
				}
				
				if(woke_handle == connections[i].process)
				{
					process_exit(i);
					break;
				}
			}
		}
	}
	
	WSACleanup();
	
	return 0;
}
