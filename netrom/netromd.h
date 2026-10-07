#ifndef	TRUE
#define	TRUE	1
#endif
#ifndef	FALSE
#define	FALSE	0
#endif

#define	NETROM_PID	0xCF

#define	NODES_SIG	0xFF

#define	CALLSIGN_LEN	7
#define	MNEMONIC_LEN	6
#define	QUALITY_LEN	1

#define	NODES_PACLEN	256
#define	ROUTE_LEN	(CALLSIGN_LEN+MNEMONIC_LEN+CALLSIGN_LEN+QUALITY_LEN)

#define NETROM_LINE_ERROR	-1
#define NETROM_LINE_EOF		0
#define NETROM_LINE_OK		1

static inline int netrom_read_line(FILE *fp, char **line, size_t *capacity)
{
	if (getline(line, capacity, fp) >= 0)
		return NETROM_LINE_OK;

	return feof(fp) ? NETROM_LINE_EOF : NETROM_LINE_ERROR;
}

struct port_struct {
	char *device;
	char *port;
	int  index;
	int  minimum_obs;
	int  default_qual;
	int  worst_qual;
	int  verbose;
};

extern struct port_struct port_list[];

extern int port_count;
extern int debug;
extern int logging;
extern int compliant;

extern ax25_address my_call;
extern ax25_address node_call;

/* In netromt.c */
extern void transmit_nodes(int, int);

/* In netromr.c */
extern void receive_nodes(unsigned char *, int, ax25_address *, int);
