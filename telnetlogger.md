# telnetlogger

## SYNOPSIS

telnetlogger <options>

## DESCRIPTION

**telnetlogger** is a small daemon that accepts incomiong Telnet
connections and logs login attempts.

There are three output formats: just the passwords, just the IP
addresses, or a CSV output containing both.

The way I run this is on a Raspberry Pi with the following
parameters:

	telnetlogger -l 2323

I then use the firewall to redirect incoming port 23 to my network
to the Raspberry Pi on port 2323.

## OPTIONS

  * `-l <port>`: A port number to listen on. Often, people will setup the
	service to listen on a high-numbered port, such as 2323, then use
	firewall rules to redirect the Telnet port 23 to this high-numbered
	port. If not specified, by default port 23 will be used. This may
	require root priveleges to run on low-numbered ports.

## OUTPUT FORMAT

The internet addresses are output in either IPv4 or IPv6 format as
appropriate. Usernames and passwords are filter non-printable characters
and some punctuation, replacing them with the standard \xXX format.

The CSV format has the columns:

	address, username, password

An example line is:

  127.0.0.1,foo,bar

## COMPATIBILITY

The tool is designed to run on Linux systems.

## AUTHORS

The tool is based on the equally named project by Robert Graham. The
original source code is available at
https://github.com/robertdavidgraham/telnetlogger.

The modifications were performed through Max Resing.
