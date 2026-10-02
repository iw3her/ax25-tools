/*
 * (c) 2002 Thomas Osterried  DL9SAU <thomas@x-berg.in-berlin.de>
 *   License: GPL. See http://www.fsf.org/
 *   Sources: http://x-berg.in-berlin.de/cgi-bin/viewcvs.cgi/ampr/axgetput/
 */

#ifndef UTIL_H
#define	UTIL_H

#include <time.h>

extern char *strlwc(char *s);
extern char *strtrim(char *s);
extern int my_read(int fd, char *s, int len_max, int *eof, char *p_break);
extern int secure_write(int fd, char *s, int len_write_left);
extern char *get_fixed_filename(char *line, long size, unsigned int msg_crc, int generate_filename);
extern time_t date_dos2unix(unsigned short time,unsigned short date);
extern void date_unix2dos(time_t unix_date,unsigned short *time, unsigned short *date);

#endif /* UTIL_H */
