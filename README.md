# ICE-9

## Introduction

ICE-9 (Insecure Command Executor for Windows 9x) is an application for remotely running commands on a Windows 9x computer.

This package consists of a server (`ice9d`) which should be run on the Windows 9x target and a client (`ice9r`) which executes commands on the server and streams the input/output.

This was hacked together over a weekend, its buggy and fragile (just like the OS it targets) and only of interest if you're a weirdo like me who wants crude SSH-like remote command execution (i.e. separate output/error streams and exit status forwarding) to remotely script things on an operating system that was obsolete a quarter of a century ago (or half a century if you compare it to UNIX).

> "I know, I'll write my own dumb little remote command executor for Windows 98 since I can't find any existing SSH servers that can work on it"
>
> *implements it, discovers you can't wait on pipes in Windows*
>
> *reworks it to use overlapped I/O, discovers 98 doesn't support [creating] named pipes*
>
> Also you can't do overlapped I/O on anonymous pipes because fuck you thats why

## Compiling

Just run the Makefile.

The client (`ice9r`) is compiled with the default C compiler, and the server (`ice9r.exe`) is compiled using a MinGW cross compiler (assumed to be `i686-w64-mingw32-gcc` by default).

## Usage

`./ice9r <IP address> [-p <port>] <executable> [<arguments> ...]`

^ This executes a command on a remote `ice9d.exe` server, encoding any arguments given into the process argument string in the "standard" Windows style.

`./ice9r <IP address> [-p <port>] <executable> [-e <command line>]`

^ This variant explicitly specifies the command line string to use as-is, for programs which have atypical argument parsing rules.
