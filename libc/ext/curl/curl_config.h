/* curl_config.h for curl 8.11.0, written by hand for mcc's aarch64 Linux libc (see mkulibs.sh).
   HTTP and HTTPS (BearSSL, TLS 1.2) only; HTTP/1.1; IPv4; resolving in the calling thread unless
   ULIBS_THREADS is defined (mkulibs.sh THREADS=1), which turns on the threaded resolver. */
#ifndef ULIBS_CURL_CONFIG_H
#define ULIBS_CURL_CONFIG_H

#define CURL_OS "aarch64-linux-mcc"
#define PACKAGE "curl"
#define PACKAGE_NAME "curl"
#define PACKAGE_BUGREPORT "none"
#define VERSION "8.11.0"
#define CURL_CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"
#define CURL_DEFAULT_SSL_BACKEND "bearssl"

/* protocols and features left out */
#define CURL_DISABLE_FTP 1
#define CURL_DISABLE_FILE 1
#define CURL_DISABLE_LDAP 1
#define CURL_DISABLE_LDAPS 1
#define CURL_DISABLE_TELNET 1
#define CURL_DISABLE_DICT 1
#define CURL_DISABLE_TFTP 1
#define CURL_DISABLE_GOPHER 1
#define CURL_DISABLE_POP3 1
#define CURL_DISABLE_IMAP 1
#define CURL_DISABLE_SMB 1
#define CURL_DISABLE_SMTP 1
#define CURL_DISABLE_RTSP 1
#define CURL_DISABLE_MQTT 1
#define CURL_DISABLE_IPFS 1
#define CURL_DISABLE_NTLM 1
#define CURL_DISABLE_KERBEROS_AUTH 1
#define CURL_DISABLE_NEGOTIATE_AUTH 1
#define CURL_DISABLE_DOH 1
#define CURL_DISABLE_ALTSVC 1
#define CURL_DISABLE_HSTS 1
#define CURL_DISABLE_NETRC 1
#define CURL_DISABLE_MIME 1
#define CURL_DISABLE_FORM_API 1
#define CURL_DISABLE_WEBSOCKETS 1
#define CURL_DISABLE_LIBCURL_OPTION 1
#define CURL_DISABLE_PROGRESS_METER 1
#define CURL_DISABLE_SHUFFLE_DNS 1
#define CURL_DISABLE_SOCKETPAIR 1
#define CURL_DISABLE_VERBOSE_STRINGS_NOT 1

#define USE_BEARSSL 1
#ifdef ULIBS_THREADS
#define USE_THREADS_POSIX 1
#define HAVE_PTHREAD_H 1
#endif

/* the compiler and the C library */
#define STDC_HEADERS 1
#define HAVE_BOOL_T 1
#define HAVE_STDBOOL_H 1
#define HAVE_LONGLONG 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_OFF_T 8
#define SIZEOF_CURL_OFF_T 8
#define SIZEOF_CURL_SOCKET_T 4
#define SIZEOF_SIZE_T 8
#define SIZEOF_TIME_T 8
#define HAVE_VARIADIC_MACROS_C99 1
#define HAVE_VARIADIC_MACROS_GCC 1

#define HAVE_ARPA_INET_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIBGEN_H 1
#define HAVE_LOCALE_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_NET_IF_H 1
#define HAVE_POLL_H 1
#define HAVE_PWD_H 1
#define HAVE_SETJMP_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_POLL_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UN_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_TERMIOS_H 1
#define HAVE_UNISTD_H 1
#define HAVE_UTIME_H 1

#define HAVE_ALARM 1
#define HAVE_BASENAME 1
#define HAVE_CLOCK_GETTIME_MONOTONIC 1
#define HAVE_FCNTL 1
#define HAVE_FCNTL_O_NONBLOCK 1
#define HAVE_FREEADDRINFO 1
#define HAVE_FSEEKO 1
#define HAVE_DECL_FSEEKO 1
#define HAVE_FTRUNCATE 1
#define HAVE_GETADDRINFO 1
#define HAVE_GETADDRINFO_THREADSAFE 1
#define HAVE_GETEUID 1
#define HAVE_GETHOSTNAME 1
#define HAVE_GETPEERNAME 1
#define HAVE_GETPPID 1
#define HAVE_GETRLIMIT 1
#define HAVE_GETSOCKNAME 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_GMTIME_R 1
#define HAVE_INET_NTOP 1
#define HAVE_INET_PTON 1
#define HAVE_IOCTL_FIONBIO 1
#define HAVE_MSG_NOSIGNAL 1
#define HAVE_PIPE 1
#define HAVE_POLL 1
#define HAVE_POLL_FINE 1
#define HAVE_RECV 1
#define HAVE_SELECT 1
#define HAVE_SEND 1
#define HAVE_SETLOCALE 1
#define HAVE_SETRLIMIT 1
#define HAVE_SIGACTION 1
#define HAVE_SIGINTERRUPT 1
#define HAVE_SIGNAL 1
#define HAVE_SIGSETJMP 1
#define HAVE_SOCKADDR_IN6_SIN6_SCOPE_ID 1
#define HAVE_SOCKET 1
#define HAVE_STRCASECMP 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR_R 1
#define HAVE_POSIX_STRERROR_R 1
#define HAVE_STRTOK_R 1
#define HAVE_STRTOLL 1
#define HAVE_STRUCT_SOCKADDR_STORAGE 1
#define HAVE_STRUCT_TIMEVAL 1
#define HAVE_SUSECONDS_T 1
#define HAVE_UTIME 1
#define HAVE_WRITABLE_ARGV 1

#define GETHOSTNAME_TYPE_ARG2 size_t
#define RECV_TYPE_ARG1 int
#define RECV_TYPE_ARG2 void *
#define RECV_TYPE_ARG3 size_t
#define RECV_TYPE_ARG4 int
#define RECV_TYPE_RETV ssize_t
#define SEND_TYPE_ARG1 int
#define SEND_QUAL_ARG2 const
#define SEND_TYPE_ARG2 void *
#define SEND_TYPE_ARG3 size_t
#define SEND_TYPE_ARG4 int
#define SEND_TYPE_RETV ssize_t
#define SELECT_TYPE_ARG1 int
#define SELECT_TYPE_ARG234 fd_set *
#define SELECT_TYPE_ARG5 struct timeval *
#define SELECT_TYPE_RETV int
#endif
