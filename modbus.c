#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/signal.h>
#include <syslog.h>
#include <pwd.h>
#include <termios.h>

#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>

/* todo:
 * - testing
 * - snmp support
 * - set time
 */

#ifndef HAVE_MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define INQSIZE		16384
#define OUTQSIZE	16384
#define MAXSERVERS	16
#define RESSIZE		1024

#define MTU		128
#define DEVID		1
#define SPEED		115200
#define	BSPEED		B115200
#define BMODE		PARENB	/* PARENB|PARODD|CSTOPB */
#ifdef RTU
#define INTERVAL	(1000*1000*8*4/SPEED+3)
#elif ZELIO
#define INTERVAL	(1000*1000*8*4/SPEED+3)
#else
#define INTERVAL	1000000 /* 1 sec */
#endif

int verbose = 0, delay = 1000, daemonize = 0, simulate = 0;
unsigned short int servport = 971, devport = 502;
struct in_addr servhost, devhost;
uid_t uid;
struct servtype {
	int sock;
	int inq_len, outq_len;
	int bye;
	int debug;
	char *inq;
	char *outq;
} serv[MAXSERVERS];
char *perlfile;
PerlInterpreter *perl;
int washup, nservers, sockfd = -1, sockclient = -1;
int serial = 0, serial_proto = 0, log_opened;
char logbuf[256];
char *devline;

void usage(void)
{
	puts("modbus wrapper");
	puts("   Usage:");
	puts("modbus [<options>] <perl-file> {<host>[:<port>]|<device>}");
	puts("   Options:");
	puts("-d               - daemonize,");
	puts("-v <n>           - set verbose level to n,");
	puts("-i <d>           - set request loop interval to <d> ms (default 1000),");
	puts("-b [<ip>:]<port> - bind to port <port> interface <ip>, default 127.0.0.1:971");
	puts("-u <user>        - drop privileges");
	puts("-s               - simulate slave device");
	return;
}

void out_line(struct servtype *serv, char *line, int len)
{
	if (len > OUTQSIZE - serv->outq_len)
		len = OUTQSIZE - serv->outq_len;
	memcpy(serv->outq + serv->outq_len, line, len);
	serv->outq_len += len;
}

void logwrite(int level, char *format, va_list ap)
{
	char *p, spaces[20];
	struct timeval now;
	struct tm *tm;
	int prio, i, len;

	vsnprintf(logbuf, sizeof(logbuf)-1, format, ap);
	if ((p=strrchr(logbuf, '\n')) != NULL && p[1] == '\0')
		*p = '\0';
	switch (level) {
		case 0:	prio = LOG_NOTICE; break;
		case 1:	prio = LOG_INFO; break;
		default:prio = LOG_DEBUG; break;
	}
	if (level <= 2) /* errors, warnings and debug(0, ...) */
		syslog(prio, "%s", logbuf);
	memset(spaces, ' ', sizeof(spaces));
	spaces[level >= sizeof(spaces) ? sizeof(spaces)-1 : level] = '\0';
	gettimeofday(&now, NULL);
	tm = localtime(&now.tv_sec);
	if (level <= verbose)
		fprintf(stderr, "%02u:%02u.%02u.%03u %s%s\n", tm->tm_hour, tm->tm_min, tm->tm_sec, now.tv_usec/1000, spaces, logbuf);
	len = strlen(logbuf);
	if (len == sizeof(logbuf)-1) {
		logbuf[sizeof(logbuf)-2] = '\0';
		len = sizeof(logbuf)-2;
	}
	strcat(logbuf, "\n");
	len++;
	for (i=0; i<nservers; i++) {
		if (serv[i].bye == 1 || serv[i].debug <= level)
			continue;
		out_line(&serv[i], spaces, level >= sizeof(spaces) ? sizeof(spaces)-1 : level);
		out_line(&serv[i], logbuf, len);
	}
}

void error(char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	logwrite(0, format, ap);
	va_end(ap);
	exit(1);
}

void warning(char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	logwrite(1, format, ap);
	va_end(ap);
}

void debug(int level, char *format, ...)
{
	va_list ap;
	int prio;

	va_start(ap, format);
	logwrite(level+2, format, ap);
	va_end(ap);
}

void initlog(void)
{
	openlog("modbus", LOG_PID, LOG_USER);
	log_opened = 1;
	warning("started");
}

int connect_client(struct in_addr host, unsigned short int port)
{
	struct sockaddr_in sin;
	int sockfd;

	sin.sin_family = AF_INET;
	sin.sin_addr = host;
	sin.sin_port = htons(port);
	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		warning("Create socket error: %s", strerror(errno));
		return -1;
	}
	if (!simulate && connect(sockfd, (struct sockaddr *)&sin, sizeof(sin))) {
		warning("Connect error: %s", strerror(errno));
		close(sockfd);
		return -1;
	}
	return sockfd;
}

int connect_serial(char *device)
{
	int sockfd;
	struct termios tio;

	sockfd = open(device, O_RDWR|O_NONBLOCK|O_NOCTTY);
	if (sockfd == -1)
		error("Cannot open serial device %s: %s", device, strerror(errno));
	memset(&tio, 0, sizeof(tio));
	tio.c_iflag=0;
	tio.c_oflag=0;
#ifdef RTU
	tio.c_cflag=CS8|CREAD|CLOCAL|BMODE;
#else /* ASCII */
	tio.c_cflag=CS7|CREAD|CLOCAL|BMODE;
#endif
	tio.c_lflag=0;
	tio.c_cc[VMIN]=1;
	tio.c_cc[VTIME]=0;
	cfsetispeed(&tio, BSPEED);
	cfsetospeed(&tio, BSPEED);
	tcsetattr(sockfd, TCSANOW, &tio);
	return sockfd;
}

void close_serv(int i)
{
	close(serv[i].sock);
	free(serv[i].outq);
	free(serv[i].inq);
	nservers--;
	memcpy(serv+i, serv+i+1, (nservers-i)*sizeof(serv[0]));
}

void sub_err(char *procname)
{
	STRLEN len;
	char *s, *p, *p1;

	p = SvPV(ERRSV, len);
	if (len) {
		s = malloc(len+1);
		strncpy(s, p, len);
		s[len] = '\0';
	} else
		s = strdup("(empty error message)");
	if ((p1 = strchr(s, '\n')) == NULL || p1[1] == '\0') {
		if (p1) *p1='\0';
		warning("Perl %s error: %s", procname, s);
	} else {
		warning("Perl %s error below:", procname);
		p = s;
		do {
			*p1 = '\0';
			warning("   %s", p);
			p = p1+1;
		} while ((p1 = strchr(p, '\n')) != NULL);
		if (*p)
			warning("  %s", p);
		free(s);
	}
}

void new_client(char *resp, int *resp_len)
{
	char *prc;
	STRLEN len;
	SV *sv;

	debug(3, "New client");
	/* call perl function hello() */
	{	dSP;
		ENTER;
		SAVETMPS;
		PUSHMARK(SP);
		PUTBACK;
		perl_call_pv("hello", G_EVAL|G_SCALAR);
		SPAGAIN;
		sv=POPs;
		prc = SvPV(sv, len);
		prc = len ? prc : "";
		debug(4, "Response: %s", prc);
		strncpy(resp, prc, *resp_len);
		*resp_len = (len > *resp_len ? 0 : *resp_len - len);
		PUTBACK;
		FREETMPS;
		LEAVE;
		if (SvTRUE(ERRSV))
			sub_err("new_client");
	}
	return;
}

int process_command(char *command, char *resp, int *resp_len, int *debuglevel)
{
	int rc = 0;
	char *prc;
	STRLEN len;
	SV *sv;

	debug(4, "Process command: %s", command);
	if ((sv = perl_get_sv("bye", TRUE)) != NULL)
		sv_setiv(sv, 0);
	if ((sv = perl_get_sv("debug", TRUE)) != NULL)
		sv_setiv(sv, *debuglevel);
	/* call perl function command() */
	{	dSP;
		ENTER;
		SAVETMPS;
		PUSHMARK(SP);
		XPUSHs(sv_2mortal(newSVpv(command, 0)));
		PUTBACK;
		perl_call_pv("command", G_EVAL|G_SCALAR);
		SPAGAIN;
		sv=POPs;
		prc = SvPV(sv, len);
		prc = len ? prc : "";
		debug(4, "Response: %s", prc);
		strncpy(resp, prc, *resp_len);
		*resp_len = (len > *resp_len ? 0 : *resp_len - len);
		PUTBACK;
		FREETMPS;
		LEAVE;
		if (SvTRUE(ERRSV))
			sub_err("command");
		else {
			sv = perl_get_sv("bye", FALSE);
			if (sv && SvTRUE(sv)) {
				debug(1, "Bye");
				rc = 1;
			}
			sv = perl_get_sv("debug", FALSE);
			if (sv) {
				*debuglevel = SvIV(sv);
			}
		}
	}
	return rc;
}

void perl_call_init(void)
{
	debug(2, "call init");
	/* call perl function init() */
	{	dSP;
		ENTER;
		SAVETMPS;
		PUSHMARK(SP);
		PUTBACK;
		perl_call_pv("init", G_EVAL|G_VOID);
		SPAGAIN;
		PUTBACK;
		FREETMPS;
		LEAVE;
		if (SvTRUE(ERRSV))
			sub_err("init");

	}
	return;
}

void perl_call_deinit(void)
{
	debug(2, "call deinit");
	/* call perl function deinit() */
	{	dSP;
		ENTER;
		SAVETMPS;
		PUSHMARK(SP);
		PUTBACK;
		perl_call_pv("deinit", G_EVAL|G_VOID);
		SPAGAIN;
		PUTBACK;
		FREETMPS;
		LEAVE;
		if (SvTRUE(ERRSV))
			sub_err("deinit");

	}
	return;
}

int perl_call_request(void)
{
	int rc = 0;
	SV *sv;

	debug(4, "call request");
	/* call perl function request() */
	{	dSP;
		ENTER;
		SAVETMPS;
		PUSHMARK(SP);
		PUTBACK;
		perl_call_pv("request", G_EVAL|G_SCALAR);
		SPAGAIN;
		sv=POPs;
		PUTBACK;
		FREETMPS;
		LEAVE;
		if (SvTRUE(ERRSV))
			sub_err("request");
		else if (!SvTRUE(sv))
			rc = -1;
	}
	return rc;
}

void perl_warn_sv(SV *sv)
{
	char *s, *p;
	STRLEN len;

	s = (char *)SvPV(sv, len);
	if (len == 0) s = "";
	if ((p = strchr(s, '\n')) != NULL)
		*p = '\0';
	warning("Perl error: %s", s);
}

static XS(perl_warn)
{
	dXSARGS;
	if (items == 1)
		perl_warn_sv (ST(0));
	XSRETURN_EMPTY;
}

int commerror(char *format, ...)
{
	va_list ap;
	char buf[256];

	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	buf[sizeof(buf)-1] = '\0';
	va_end(ap);
	warning("%s", buf);
	if (serial)
		usleep(1000000); /* 1 sec */
	else {
		close(sockclient);
		sockclient = -1;
	}
	return -1;
}

int send_serial(int sock, char *buf, int num)
{
	fd_set fd;
	struct timeval tv;
	int n, r, i;
	char debugstr[256];

	n = num;
	debugstr[0] = debugstr[sizeof(debugstr)-1] = '\0';
	while (n > 0) {
		FD_ZERO(&fd);
		FD_SET(sock, &fd);
		tv.tv_sec = 0;
		tv.tv_usec = INTERVAL;
		r = select(sock+1, NULL, &fd, NULL, &tv);
		if (r <= 0) return r;
		r = write(sock, buf, n);
		if (r <= 0) return r;
		for (i=0; i<r; i++) {
#if 0
			if (debugstr[0])
				strncat(debugstr, ", ", sizeof(debugstr)-strlen(debugstr)-1);
			snprintf(debugstr+strlen(debugstr), sizeof(debugstr)-strlen(debugstr)-1, "0x%02X", (unsigned char)buf[i]);
#else
			snprintf(debugstr+strlen(debugstr), sizeof(debugstr)-strlen(debugstr)-1, "%c", buf[i]);
#endif
		}
		buf += r;
		n -= r;
	}
	if (debugstr[0])
		debug(6, "Send to serial: %s", debugstr);
	return num;
}

int recv_serial(int sock, char *buf, int num)
{
	fd_set fd;
	struct timeval tv;
	int n, r, i;
	char debugstr[256];

	n = num;
	debugstr[0] = debugstr[sizeof(debugstr)-1] = '\0';
	while (n > 0) {
		FD_ZERO(&fd);
		FD_SET(sock, &fd);
		tv.tv_sec = 0;
		tv.tv_usec = 30000; /* 30 ms or INTERVAL? */
		r = select(sock+1, &fd, NULL, NULL, &tv);
		if (r < 0) return r;
		if (r == 0) {
			if (debugstr[0])
				debug(6, "Recv from serial: %s", debugstr);
			return num-n;
		}
		r = read(sock, buf, n);
		if (r <= 0) return r;
		for (i=0; i<r; i++) {
#if 0
			if (debugstr[0])
				strncat(debugstr, ", ", sizeof(debugstr)-strlen(debugstr)-1);
			snprintf(debugstr+strlen(debugstr), sizeof(debugstr)-strlen(debugstr)-1, "0x%02X", (unsigned char)buf[i]);
#else
			snprintf(debugstr+strlen(debugstr), sizeof(debugstr)-strlen(debugstr)-1, "%c", buf[i]);
#endif
		}
		buf += r;
		n -= r;
	}
	if (debugstr[0])
		debug(6, "Recv from serial: %s", debugstr);
	return num;
}

/* Table of CRC values for high-order byte */
const unsigned char auchCRCHi[] = {
	0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 
	0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 
	0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 
	0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 
	0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 
	0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 
	0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 
	0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 
	0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 
	0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 
	0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 
	0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 
	0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 
	0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 
	0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 
	0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 
	0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 
	0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 
	0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 
	0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 
	0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 
	0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 
	0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 
	0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 
	0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 
	0x80, 0x41, 0x00, 0xC1, 0x81, 0x40
};

/* Table of CRC values for low-order byte */
const char auchCRCLo[] = {
	0x00, 0xC0, 0xC1, 0x01, 0xC3, 0x03, 0x02, 0xC2, 0xC6, 0x06, 
	0x07, 0xC7, 0x05, 0xC5, 0xC4, 0x04, 0xCC, 0x0C, 0x0D, 0xCD, 
	0x0F, 0xCF, 0xCE, 0x0E, 0x0A, 0xCA, 0xCB, 0x0B, 0xC9, 0x09, 
	0x08, 0xC8, 0xD8, 0x18, 0x19, 0xD9, 0x1B, 0xDB, 0xDA, 0x1A, 
	0x1E, 0xDE, 0xDF, 0x1F, 0xDD, 0x1D, 0x1C, 0xDC, 0x14, 0xD4, 
	0xD5, 0x15, 0xD7, 0x17, 0x16, 0xD6, 0xD2, 0x12, 0x13, 0xD3, 
	0x11, 0xD1, 0xD0, 0x10, 0xF0, 0x30, 0x31, 0xF1, 0x33, 0xF3, 
	0xF2, 0x32, 0x36, 0xF6, 0xF7, 0x37, 0xF5, 0x35, 0x34, 0xF4, 
	0x3C, 0xFC, 0xFD, 0x3D, 0xFF, 0x3F, 0x3E, 0xFE, 0xFA, 0x3A, 
	0x3B, 0xFB, 0x39, 0xF9, 0xF8, 0x38, 0x28, 0xE8, 0xE9, 0x29, 
	0xEB, 0x2B, 0x2A, 0xEA, 0xEE, 0x2E, 0x2F, 0xEF, 0x2D, 0xED, 
	0xEC, 0x2C, 0xE4, 0x24, 0x25, 0xE5, 0x27, 0xE7, 0xE6, 0x26, 
	0x22, 0xE2, 0xE3, 0x23, 0xE1, 0x21, 0x20, 0xE0, 0xA0, 0x60, 
	0x61, 0xA1, 0x63, 0xA3, 0xA2, 0x62, 0x66, 0xA6, 0xA7, 0x67, 
	0xA5, 0x65, 0x64, 0xA4, 0x6C, 0xAC, 0xAD, 0x6D, 0xAF, 0x6F, 
	0x6E, 0xAE, 0xAA, 0x6A, 0x6B, 0xAB, 0x69, 0xA9, 0xA8, 0x68, 
	0x78, 0xB8, 0xB9, 0x79, 0xBB, 0x7B, 0x7A, 0xBA, 0xBE, 0x7E, 
	0x7F, 0xBF, 0x7D, 0xBD, 0xBC, 0x7C, 0xB4, 0x74, 0x75, 0xB5, 
	0x77, 0xB7, 0xB6, 0x76, 0x72, 0xB2, 0xB3, 0x73, 0xB1, 0x71, 
	0x70, 0xB0, 0x50, 0x90, 0x91, 0x51, 0x93, 0x53, 0x52, 0x92, 
	0x96, 0x56, 0x57, 0x97, 0x55, 0x95, 0x94, 0x54, 0x9C, 0x5C, 
	0x5D, 0x9D, 0x5F, 0x9F, 0x9E, 0x5E, 0x5A, 0x9A, 0x9B, 0x5B, 
	0x99, 0x59, 0x58, 0x98, 0x88, 0x48, 0x49, 0x89, 0x4B, 0x8B, 
	0x8A, 0x4A, 0x4E, 0x8E, 0x8F, 0x4F, 0x8D, 0x4D, 0x4C, 0x8C, 
	0x44, 0x84, 0x85, 0x45, 0x87, 0x47, 0x46, 0x86, 0x82, 0x42, 
	0x43, 0x83, 0x41, 0x81, 0x80, 0x40
};

int crc(char *str, int len)
{
	unsigned char uchCRCHi = 0xFF, uchCRCLo = 0xFF;
	unsigned uIndex;

	while (len--) {
		uIndex = uchCRCHi ^ *str++;
		uchCRCHi = uchCRCLo ^ auchCRCHi[uIndex];
		uchCRCLo = auchCRCLo[uIndex];
	}
	return (uchCRCHi << 8 | uchCRCLo);
}

static char lrc(char *str, int len)
{
	unsigned char uchLRC = 0;
	
	while (len--)
		uchLRC += (unsigned char)*str++;
	return -((char)uchLRC)+1;
}

static char digit[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };

void hexbyte(char *buf, int byte)
{
	*buf++ = digit[(byte & 0xf0) >> 4];
	*buf = digit[byte & 0x0f];
}

char unhexdigit(char digit)
{
	if (isdigit(digit)) return digit-'0';
	if (digit>='A' && digit<='F') return digit-'A'+10;
	return 0;
}

char unhexbyte(char *buf)
{
	return (unhexdigit(buf[0])<<4) + unhexdigit(buf[1]);
}

char *mem2hex(char *buf, int bufsize)
{
	static char hexstr[256];
	int i;

	hexstr[0] = hexstr[sizeof(hexstr)-1] = '\0';
	while (bufsize > 0) {
		if (hexstr[0])
			strncat(hexstr, " ", sizeof(hexstr)-strlen(hexstr)-1);
		snprintf(hexstr+strlen(hexstr), sizeof(hexstr)-strlen(hexstr)-1, "0x%02X (%d)", (unsigned char)*buf,*buf);
		buf++;
		bufsize--;
	}
	return hexstr;
}

int modbus_comm(char *request, int reqsize, char *response, int respsize)
{
	fd_set fd;
	struct timeval tv;
	int n;
	char buf[MTU+7];
	int icrc;

	if (reqsize>MTU || respsize>MTU) {
		warning("Too large pkt, %d bytes (max %d)", reqsize>respsize ? reqsize : respsize, MTU);
		return -1;
	}
	if (simulate) {
		response[0] = request[0];
		if (request[0] == 3 || request[0] == 23) { /* read or read/write registers */
			response[1] = (serial_proto ? request[5] : 2*request[4]);
			n = htons(1); memcpy(response+2, &n, 2);
			n = htons(2); memcpy(response+4, &n, 2);
			n = htons(3); memcpy(response+6, &n, 2);
			n = htons(4); memcpy(response+8, &n, 2);
		} else if (request[0] == 16) {
			if (serial_proto) {
				response[1] = 0;
				response[2] = 0;
				response[3] = 0xff;
				response[4] = request[4];
				response[5] = request[5];
			} else {
				response[1] = 0;
				response[2] = 0;
				response[3] = 0;
				response[4] = 4;
			}
		}
		return 0;
	}
	if (sockclient == -1)
		return -1;
	if (serial) {
		usleep(INTERVAL);
#ifdef RTU
		buf[0] = DEVID;
		memcpy(buf+1, request, reqsize);
		icrc = crc(buf, reqsize+1);
		buf[reqsize+1] = icrc & 0xff;
		buf[reqsize+2] = icrc >> 8;
		reqsize += 3;
#else /* ASCII */
		if (reqsize*2>MTU || respsize*2>MTU) {
			warning("Too large pkt, %d bytes (max %d)", reqsize>respsize ? reqsize : respsize, MTU);
			return -1;
		}
		/* calculate LRC */
		buf[0] = DEVID;
		memcpy(buf+1, request, reqsize);
		icrc = lrc(buf, reqsize+1);
		buf[0] = ':';
		hexbyte(buf+1, DEVID);
		for (n=0; n<reqsize; n++)
			hexbyte(buf+n*2+3, request[n]);
		hexbyte(buf+reqsize*2+3, icrc);
		buf[reqsize*2+5]='\r';
		buf[reqsize*2+6]='\n';
		reqsize=reqsize*2+7;
#endif
		n = send_serial(sockclient, buf, reqsize);
	} else {
		memset(buf, 0, 7);
		buf[5] = (char)reqsize+1;
		buf[6] = DEVID;
		memcpy(buf+7, request, reqsize);
		reqsize += 7;
		FD_ZERO(&fd);
		FD_SET(sockclient, &fd);
		tv.tv_sec = 0;
		tv.tv_usec = 30 * 1000;
		n = select(sockclient+1, NULL, &fd, NULL, &tv);
		if (n == -1)
			return commerror("Error in communication with device: %s", strerror(errno));
		if (n == 0)
			return commerror("Device communication timeout for writing");
		debug(6, "send to device: %s", mem2hex(buf, reqsize));
		n = send(sockclient, buf, reqsize, MSG_NOSIGNAL|MSG_DONTWAIT);
	}
	if (n < 0)
		return commerror("Error in communication with device: %s", strerror(errno));
	if (n < reqsize)
		return commerror("Device communication timeout (sent partial)");
	FD_ZERO(&fd);
	FD_SET(sockclient, &fd);
	tv.tv_sec = 0;
	tv.tv_usec = (serial ? 300 : 30) * 1000;
	n = select(sockclient+1, &fd, NULL, NULL, &tv);
	if (n == -1)
		return commerror("Error in communication with device: %s", strerror(errno));
	if (n == 0)
		return commerror("Device communication timeout for reading");
	if (serial) {
		unsigned int icrc;
	
#ifdef RTU
		n = recv_serial(sockclient, buf, 5);  /* slave-id(1), func(1), exeption(1), crc(2) */
		if (n < 0)
			return commerror("Error in communication with device: %s", strerror(errno));
		if (n < 5)
			return commerror("Device communication timeout (read partial)");
		if (buf[1] & 0x80) {
			/* error reported */
			icrc = ntohs(buf[3] + buf[4]*256);
			if (icrc != crc(buf, 3))
				return commerror("CRC error");
			warning("Modbus exception, function %d, code %d", request[0], buf[2]);
			return -1;
		}
		n = recv_serial(sockclient, buf+5, respsize-2);
		if (n < 0)
			return commerror("Error in communication with device: %s", strerror(errno));
		if (n < respsize-2)
			return commerror("Device communication timeout (read2 partial)");
		icrc = ntohs(buf[respsize+1] + buf[respsize+2]*256);
		if (icrc != crc(buf, respsize+1))
			return commerror("CRC error");
		if (buf[1] != request[0])
			return commerror("Protocol error in device communication");
		memcpy(response, buf+1, respsize);
#else
		n = recv_serial(sockclient, buf, 11); /* ':', slave-id (2), func(2), exeption(2), lrc(2), crlf */
		if (n < 0)
			return commerror("Error in communication with device: %s", strerror(errno));
		if (n < 11)
			return commerror("Device communication timeout (read partial)");
		if (buf[0] != ':')
			return commerror("Protocol error in device communication (no frame start marker)");
		buf[0] = unhexbyte(buf+1); /* device id */
		buf[1] = unhexbyte(buf+3); /* function number */
		buf[2] = unhexbyte(buf+5);
		buf[3] = unhexbyte(buf+7);
		if (buf[1] & 0x80) {
			/* error reported */
			if (buf[3] != lrc(buf, 3))
				return commerror("CRC error");
			warning("Modbus exception, function %d, code %d", request[0], buf[2]);
			return -1;
		}
		buf[4] = unhexbyte(buf+9);
		n = recv_serial(sockclient, buf+10, (respsize-4+2)*2); /* lrc, crlf */
		if (n < 0)
			return commerror("Error in communication with device: %s", strerror(errno));
		if (n < (respsize-2)*2) {
			warning("read %d bytes, needed %d", n, respsize*2-4);
			return commerror("Device communication timeout (read2 partial)");
		}
		if (buf[respsize*2+4]!='\r' || buf[respsize*2+5]!='\n')
			return commerror("Protocol error in device communication (no frame end marker)");
		for (n=5; n<respsize+2; n++)
			buf[n] = unhexbyte(buf+n*2);
		if (buf[respsize+1] != lrc(buf, respsize+1))
			return commerror("CRC error");
		if (buf[1] != request[0])
			return commerror("Protocol error in device communication (bad function code in response)");
		memcpy(response, buf+1, respsize);
#endif
		usleep(INTERVAL);
	} else {
		n = recv(sockclient, buf, 8, MSG_NOSIGNAL|MSG_DONTWAIT);
		if (n < 0)
			return commerror("Error in communication with device: %s", strerror(errno));
		debug(6, "recv from device: %s", mem2hex(buf, n));
		if (n < 8)
			return commerror("Device communication timeout");
		/* ok, check response */
		if (buf[0]!=0 || buf[1]!=0 || buf[2]!=0 || buf[3]!=0 || buf[4]!=0)
			return commerror("Protocol error in communication with device");
		if (buf[5]>respsize+1 || buf[5] == 0 || buf[6] != 1 || (buf[7] & 0x7f) != request[0])
			return commerror("Protocol error in communication with device");
		response[0] = buf[7];
		n = recv(sockclient, response+1, respsize-1, MSG_NOSIGNAL|MSG_DONTWAIT);
		if (n < 0)
			return commerror("Error in communication with device: %s", strerror(errno));
		debug(6, "recv from device: %s", mem2hex(response+1, n));
		if (n < respsize-1 && (n < 1 || buf[7] == request[0]))
			return commerror("Device communication timeout");
	}
	if (response[0] != request[0]) {
		warning("Modbus exception, function %d, code %d", request[0], response[1]);
		return -1;
	}
	return 0;
}

static XS(modbus_write_registers)
{
	dXSARGS;
	char request[] = { 16, 0, 0, 0, 4, 8, 0, 0, 0, 0, 0, 0, 0, 0 };
	char response[6];
	unsigned short r;
	unsigned off, num, i;
	char debugstr[256];

	/* fetch registers from stack */
	if (items < 2) {
		warning("modbus_write_registers: wrong number of params (need >=2, exists %d)", items);
		XSRETURN_UNDEF;
	}
	off = SvUV(ST(0));
	num = SvUV(ST(1));
	if (items != num+2) {
		warning("modbus_write_registers: wrong number of params (need %d, exists %d)", num+2, items);
		XSRETURN_UNDEF;
	}
	if (off > 100 || num > 4) {
		warning("modbus_write_registers: incorrect params");
		XSRETURN_UNDEF;
	}
	memset(request, 0, sizeof(request));
	request[0] = 0x10;
	if (serial_proto) {
		request[3] = 0xFF;
		request[4] = off;
	} else {
		request[2] = off;
		request[4] = num;
	}
	request[5] = 2*num;
	debugstr[0] = debugstr[sizeof(debugstr)-1] = '\0';
	for (i=0; i<num; i++) {
		r = htons(SvUV(ST(i+2)));
		memcpy(request+6+2*i,  &r, 2);
		strncat(debugstr, " ", sizeof(debugstr)-strlen(debugstr)-1);
		snprintf(debugstr+strlen(debugstr), sizeof(debugstr)-strlen(debugstr)-1, "0x%02X (%d)", SvUV(ST(i+2)), (short)SvUV(ST(i+2)));
	}
	debug(2, "modbus write registers at offset %d:%s", off, debugstr);
	if (modbus_comm(request, 6+2*num, response, serial_proto ? 6 : 5))
		XSRETURN_UNDEF;
	if (serial_proto) {
		if (response[1] == 0 && response[2] == 0 && response[3] == (char)0xFF && response[4] == (char)off && response[5] == num*2) {
			XSRETURN_IV(1);
		}
	} else {
		if (response[1] == 0 && response[2] == (char)off && response[3] == 0 && response[4] == num) {
			XSRETURN_IV(1);
		}
	}
	warning("Unexpected answer to write registers request");
	XSRETURN_UNDEF;
}

static XS(modbus_read_registers)
{
	dXSARGS;
	char request[6];
	char response[2+2*4];
	unsigned short r;
	unsigned off, num, i;
	char debugstr[256];

	if (items != 2) {
		warning("modbus_read_registers: wrong number of params (need 2, exists %d)", items);
		XSRETURN_UNDEF;
	}
	debug(3, "call modbus read registers");
	num = POPi;
	off = POPi;
	if (off>100 || num > 4) {
		warning("modbus_read_registers: incorrect params (off %d, num %d)", off, num);
		XSRETURN_UNDEF;
	}
	memset(request, 0, sizeof(request));
	request[0] = 3;
	if (serial_proto) {
		request[3] = 0xff;
		request[4] = (char)off;
		request[5] = (char)(2*num);
	} else {
		request[2] = (char)off;
		request[4] = (char)num;
	}
	if (modbus_comm(request, serial_proto ? 6 : 5, response, 2+2*num))
		XSRETURN_UNDEF;
	if (response[1] != 2*num) {
		warning("modbus_read_registers: protocol error");
		XSRETURN_UNDEF;
	}
	/* put registers as return array */
	debugstr[0] = debugstr[sizeof(debugstr)-1] = '\0';
	for (i=0; i<num; i++) {
		memcpy(&r, response+2+2*i, 2); r = (short)ntohs(r); XPUSHs(sv_2mortal(newSViv((short)r)));
		strncat(debugstr, " ", sizeof(debugstr)-strlen(debugstr)-1);
		snprintf(debugstr+strlen(debugstr), sizeof(debugstr)-strlen(debugstr)-1, "0x%02X (%d)", r, (short)r);
	}
	debug(2, "modbus read registers at offset %d:%s", off, debugstr);
	XSRETURN(num);
}

static XS(modbus_read_write_registers)
{
	dXSARGS;
	char request[] = { 23, 0, 0, 0, 4, 0, 0, 0, 4, 8, 0, 0, 0, 0, 0, 0, 0, 0 };
	char response[2+2*4];
	unsigned short r;
	unsigned roff, rnum, woff, wnum, i;

	/* fetch registers from stack */
	if (items < 4) {
		warning("modbus_read_write_registers: wrong number of params (need >=4, exists %d)", items);
		XSRETURN_UNDEF;
	}
	debug(3, "call modbus read and write registers");
	roff = SvUV(ST(0));
	rnum = SvUV(ST(1));
	woff = SvUV(ST(2));
	wnum = SvUV(ST(3));
	if (roff>100 || rnum>4 || woff>100 || wnum>4) {
		warning("modbus_read_write_registers: incorrect params");
		XSRETURN_UNDEF;
	}
	if (items != wnum+4) {
		warning("modbus_read_write_registers: wrong number of params (need %d, exists %d)", wnum+4, items);
		XSRETURN_UNDEF;
	}
	request[2] = roff;
	request[4] = rnum;
	request[6] = woff;
	request[8] = wnum;
	request[9] = 2*wnum;
	for (i=0; i<wnum; i++) {
		r = htons(SvUV(ST(i+4)));
		memcpy(request+10+2*i,  &r, 2);
	}
	if (modbus_comm(request, sizeof(request), response, sizeof(response)))
		XSRETURN_UNDEF;
	if (response[1] != 2*rnum) {
		warning("modbus_read_registers: protocol error");
		XSRETURN_UNDEF;
	}
	/* put registers as return array */
	for (i=0; i<rnum; i++)
		memcpy(&r, response+2+2*i, 2); XPUSHs(sv_2mortal(newSViv((short)ntohs(r))));
	XSRETURN(rnum);
}

static XS(perl_logwrite)
{
	dXSARGS;
	STRLEN len;
	char *line;
	unsigned int level;

	/* fetch registers from stack */
	if (items != 2) {
		warning("logwrite: wrong number of params (need 1, exists %d)", items);
		XSRETURN_EMPTY;
	}
	level = SvUV(ST(0));
	line = SvPV(ST(1), len);
	if (len == 0) line = "";
	if (level == 0)
		warning("perl: %s", line);
	else
		debug(level-1, "perl: %s", line);
	XSRETURN_EMPTY;
}

XS(boot_DynaLoader);

#ifdef pTHX
static void xs_init(pTHX)
#else
static void xs_init(void)
#endif
{
	static char *file = __FILE__;
	dXSUB_SYS;
	newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, file);
	newXS("logwrite", perl_logwrite, file);
	newXS("modbus_warn", perl_warn, file);
	newXS("modbus_write_registers", modbus_write_registers, file);
	newXS("modbus_read_registers", modbus_read_registers, file);
	newXS("modbus_read_write_registers", modbus_read_write_registers, file);
}

void hup(int signo)
{
	washup=1;
}

PerlInterpreter *perl_init(char *perlfile, int first)
{
	int rc, i;
	SV *sv;
	char cmd[256];
	char *perlargs[] = {"", "-e", "0", NULL};
	char **perlargv = (char **)perlargs;
	char **perlenv = { NULL };
	PerlInterpreter *perl;

#ifdef PERL_SYS_INIT3
	if (first)
		PERL_SYS_INIT3(&i, &perlargv, &perlenv);
#endif
	perl = perl_alloc();
#ifdef PERL_SET_CONTEXT
	PERL_SET_CONTEXT(perl);
#endif
	PL_perl_destruct_level = 1;
	perl_construct(perl);
	PL_origalen = 1;
	if (perl_parse(perl, xs_init, 3, perlargv, NULL)) {
		perl_destruct(perl);
		perl_free(perl);
#ifdef PERL_SYS_TERM
		PERL_SYS_TERM();
#endif
		warning("Can't parse %s", perlfile);
		return NULL;
	}
	/* Set warn and die hooks */
	if (PL_warnhook) SvREFCNT_dec (PL_warnhook);
	if (PL_diehook ) SvREFCNT_dec (PL_diehook );
	PL_warnhook = newRV_inc ((SV*) perl_get_cv ("modbus_warn", TRUE));
	PL_diehook  = newRV_inc ((SV*) perl_get_cv ("modbus_warn", TRUE));
	PL_exit_flags |= PERL_EXIT_DESTRUCT_END;
	strncpy(cmd, "do '", sizeof(cmd)-1);
	strncat(cmd, perlfile, sizeof(cmd)-1);
	strncat(cmd, "'; $@ ? $@ : '';", sizeof(cmd)-1);
	cmd[sizeof(cmd)-1] = '\0';
	sv = perl_eval_pv (cmd, TRUE);
	rc = 0;
	if (!SvPOK(sv)) {
		warning("Syntax error in internal perl expression: %s", cmd);
		rc = 1;
	} else if (SvTRUE (sv)) {
		perl_warn_sv(sv);
		rc = 1;
	}
	if (rc) {
		perl_destruct(perl);
		perl_free(perl);
#ifdef PERL_SYS_TERM
		PERL_SYS_TERM();
#endif
		return NULL;
	}
	if (!perl_get_cv("command", FALSE)) {
		warning("Perl function command() not defined");
		rc = 1;
	}
	if (!perl_get_cv("init", FALSE)) {
		warning("Perl function init() not defined");
		rc = 1;
	}
	if (!perl_get_cv("deinit", FALSE)) {
		warning("Perl function deinit() not defined");
		rc = 1;
	}
	if (!perl_get_cv("request", FALSE)) {
		warning("Perl function request() not defined");
		rc = 1;
	}
	if (!perl_get_cv("hello", FALSE)) {
		warning("Perl function hello() not defined");
		rc = 1;
	}
	if (rc) {
		perl_destruct(perl);
		perl_free(perl);
#ifdef PERL_SYS_TERM
		PERL_SYS_TERM();
#endif
		return NULL;
	}
	return perl;
}

void perl_reload(char *perlfile)
{
#ifdef PERL_MULTIPLICITY
	PerlInterpreter *newperl;

	newperl = perl_init(perlfile, 0);
	if (newperl) {
		PERL_SET_CONTEXT(perl);
		perl_destruct(perl);
		perl_free(perl);
		perl = newperl;
		PERL_SET_CONTEXT(perl);
		perl_call_init();
	} else {
		PERL_SET_CONTEXT(perl);
	}
#else
	PL_perl_destruct_level = 1;
	perl_destruct(perl);
	perl_free(perl);
	perl = perl_init(perlfile, 0);
	if (perl == NULL)
		error("Error in perl module, cannot continue");
#endif
}

void exitfunc(void)
{
	int i;

	debug(0, "shutdown");
	if (sockclient != -1) {
		close(sockclient);
		sockclient = -1;
	}
	if (perl) {
		perl_call_deinit();
		perl_destruct(perl);
		perl_free(perl);
	}
	for (i=0; i<nservers; i++)
		close(serv[i].sock);
	nservers = 0;
	if (sockfd != -1) {
		close(sockfd);
		sockfd = -1;
	}
	closelog();
}

int get_host(char *str, struct in_addr *host)
{
	struct hostent *hp;

	if (!isdigit(str[0]) || (host->s_addr = inet_addr(str)) == INADDR_NONE) {
		hp = gethostbyname(str);
		if (hp)
			memcpy(host, hp->h_addr, sizeof(*host)); /* if many IP-addresses use random */
		else
			error("%s: %s", str, strerror(errno));
	}
	return 0;
}

int get_port(char *str, unsigned short *port)
{
	struct servent *sp;

	sp = getservbyname(str, "tcp");
	if (sp)
		*port = ntohs(sp->s_port);
	else if (isdigit(str[0]))
		*port = (unsigned short)atoi(str);
	else
		error("Unknown port %s", str);
	return 0;
}

int get_host_port(char *str, struct in_addr *host, unsigned short *port, int onlyhost)
{
	char *p;

	p = strchr(str, ':');
	if (p) {
		*p++ = '\0';
		get_host(str, host);
		get_port(p, port);
	} else if (onlyhost)
		get_host(str, host);
	else
		get_port(str, port);
	return 0;
}

int main(int argc, char *argv[])
{
	int i, k, n;
	struct timeval tv, tv_now, next_req;
	char *p;
	struct sockaddr_in serv_addr;
	struct passwd *pw;

	devhost.s_addr = servhost.s_addr = inet_addr("127.0.0.1");
	while ((i = getopt(argc, argv, "b:i:dv:u:s")) != -1)
	{
		switch (i) {
			case 'd':
				daemonize = 1;
				break;
			case 'v':
				verbose = atoi(optarg);
				break;
			case 'i':
				delay = atoi(optarg);
				break;
			case 'b':
				get_host_port(optarg, &servhost, &servport, 0);
				break;
			case 'u':
				if ((pw = getpwnam(optarg)) != NULL)
					uid = pw->pw_uid;
				else if (isdigit(optarg[0]))
					uid = atoi(optarg);
				else
					error("%s: unknown user", optarg);
				break;
			case 's':
				simulate = 1;
				break;
			default:
				fprintf(stderr, "Unknown option -%c ignored\n", i);
				break;
		}
	}
	if (optind == argc-2) {
		perlfile = argv[optind];
		if (strncmp(argv[optind+1], "/dev/", 5) == 0) {
			serial = 1;
			devline = argv[optind+1];
#ifdef ZELIO
			serial_proto = 1;	/* Zelio proprietary modbus-like protocol */
#endif
		} else
			get_host_port(argv[optind+1], &devhost, &devport, 1);
	} else {
		usage();
		return 0;
	}

	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
		error("Create socket error: %s", strerror(errno));
	i = 1;
	if (setsockopt (sockfd, SOL_SOCKET, SO_REUSEADDR, (char *)&i, sizeof i) == -1)
		warning("setsockopt error: %s", strerror(errno));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr = servhost;
	serv_addr.sin_port = htons(servport);
	if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof serv_addr) != 0) {
		close(sockfd);
		error("Bind error: %s", strerror(errno));
	}

	sockclient = serial ? connect_serial(devline) : connect_client(devhost, devport);
	if (sockclient == -1) { 
		close(sockfd);
		error("Controller not found");
	}

	/* drop privs */
	if (uid)
		if (setuid(uid))
			error("Cannot drop privileges: %s", strerror(errno));

	if ((perl = perl_init(perlfile, 1)) == NULL)
		exit(1);
	perl_call_init();

	if (listen(sockfd, 5)) {
		close(sockfd);
		error("Listen error: %s", strerror(errno));
	}

	if (daemonize) {
		if (daemon(0, 0))
			error("Daemonize error: %s", strerror(errno));
	}
	initlog();
	atexit(exitfunc);
#ifndef HAVE_MSG_NOSIGNAL
	signal(SIGPIPE, SIG_IGN);
#endif
	signal(SIGHUP, hup);

	/* Main loop */
	next_req.tv_sec = next_req.tv_usec = 0;
	while (1) {
		fd_set r, w;
		int maxfd, newsock;

		FD_ZERO(&r);
		FD_ZERO(&w);
		maxfd = 0;
		if (nservers < MAXSERVERS) {
			FD_SET(sockfd, &r);
			if (maxfd <= sockfd)
				maxfd = sockfd+1;
		}
		for (i=0; i<nservers; i++) {
			if (serv[i].bye == 0)
				FD_SET(serv[i].sock, &r);
			if (serv[i].outq_len)
				FD_SET(serv[i].sock, &w);
			if (maxfd <= serv[i].sock)
				maxfd = serv[i].sock+1;
		}
		gettimeofday(&tv_now, NULL);
		if (next_req.tv_sec < tv_now.tv_sec ||
		    (next_req.tv_sec == tv_now.tv_sec && next_req.tv_usec <= tv_now.tv_usec)) {
			tv.tv_sec = tv.tv_usec = 0;
			if (next_req.tv_sec == 0) { /* first init */
				next_req.tv_sec = tv_now.tv_sec;
				next_req.tv_usec = tv_now.tv_usec;
			}
		} else {
			if (next_req.tv_usec >= tv_now.tv_usec) {
				tv.tv_sec = next_req.tv_sec - tv_now.tv_sec;
				tv.tv_usec = next_req.tv_usec - tv_now.tv_usec;
			} else {
				tv.tv_sec = next_req.tv_sec - tv_now.tv_sec - 1;
				tv.tv_usec = next_req.tv_usec + 1000000 - tv_now.tv_usec;
			}
		}
		if ((n = select(maxfd, &r, &w, NULL, &tv)) == -1) {
			if (errno == EINTR)
				continue;
			error("Select error: %s", strerror(errno));
		}
		if (n) {
			for (i=0; i<nservers; i++) {
				if (serv[i].outq_len && FD_ISSET(serv[i].sock, &w)) {
					n = send(serv[i].sock, serv[i].outq, serv[i].outq_len, MSG_NOSIGNAL);
					if (n == -1) {
						if (errno != EAGAIN && errno != EINTR) {
							warning("error write to socket: %s", strerror(errno));
							close_serv(i);
							i--;
						}
						continue;
					}
					if (n>0 && n<serv[i].outq_len)
						memcpy(serv[i].outq, serv[i].outq+n, serv[i].outq_len-n);
					serv[i].outq_len -= n;
					if (serv[i].outq_len == 0 && serv[i].bye) {
						close_serv(i);
						i--;
						debug(2, "Session closed");
						continue;
					}
				}
				if (FD_ISSET(serv[i].sock, &r)) {
					if (serv[i].inq_len == INQSIZE) {
						warning("Inbound queue overflow");
						close_serv(i);
						i--;
						continue;
					}
					n = read(serv[i].sock, serv[i].inq+serv[i].inq_len, INQSIZE-serv[i].inq_len);
					if (n == -1) {
						if (errno != EAGAIN && errno != EINTR) {
							warning("error read from socket: %s", strerror(errno));
							close_serv(i);
							i--;
						}
						continue;
					}
					if (n == 0) {
						debug(0, "session closed");
						close_serv(i);
						i--;
						continue;
					}
					serv[i].inq_len += n;
					if (memchr(serv[i].inq, '\0', serv[i].inq_len) != NULL) {
						warning("Zero byte in input");
						close_serv(i);
						i--;
						continue;
					}
					if (washup) {
						debug(0, "sighup received, perl reloaded");
						perl_reload(perlfile);
						washup = 0;
					}
					while ((p = memchr(serv[i].inq, '\n', serv[i].inq_len)) != NULL) {
						char res[RESSIZE];
						int reslen;

						*p = '\0';
						if (p > serv[i].inq && *(p-1) == '\r')
							*(p-1) = '\0';
						reslen = sizeof(res);
						n = process_command(serv[i].inq, res, &reslen, &serv[i].debug);
						out_line(serv+i, res, reslen);
						memcpy(serv[i].inq, p+1, serv[i].inq_len-(p+1-serv[i].inq));
						serv[i].inq_len -= p+1-serv[i].inq;
						if (n) /* quit command */
							serv[i].bye = 1;
					}
				}
			}
			if (FD_ISSET(sockfd, &r)) {
#ifdef HAVE_ACCEPT4
				if ((newsock = accept4(sockfd, NULL, NULL, SOCK_NONBLOCK)) == -1)
#else
				if ((newsock = accept(sockfd, NULL, NULL)) == -1)
#endif
				{
					if (errno != EAGAIN && errno != ECONNABORTED && errno != EINTR)
						error("accept error: %s", strerror(errno));
					warning("accept error: %s", strerror(errno));
					continue;
				}
				serv[nservers].sock = newsock;
				serv[nservers].outq_len = 0;
				serv[nservers].outq = malloc(OUTQSIZE);
				serv[nservers].inq_len = 0;
				serv[nservers].inq = malloc(INQSIZE);
				serv[nservers].bye = 0;
				serv[nservers].debug = 0;
				nservers++;
				n = OUTQSIZE;
				new_client(serv[i].outq, &n);
				serv[i].outq_len = OUTQSIZE-n;
			}
		} else {
			/* timeout, send request to controller */
			if (sockclient == -1 && !serial) {
				sockclient = connect_client(devhost, devport);
				if (sockclient == -1) continue;
			}
			/* call perl function request() */
			if (washup) {
				debug(0, "sighup received, perl reloaded");
				perl_reload(perlfile);
				washup = 0;
			}
			perl_call_request();
			next_req.tv_usec += (delay%1000)*1000;
			next_req.tv_sec += delay/1000 + next_req.tv_usec/1000000;
			next_req.tv_usec %= 1000000;
			if (next_req.tv_sec < tv_now.tv_sec ||
			    (next_req.tv_sec == tv_now.tv_sec && next_req.tv_usec <= tv_now.tv_usec)) {
				next_req.tv_sec = tv_now.tv_sec;
				next_req.tv_usec = tv_now.tv_usec;
			}
		}
	}
	return 0;
}

