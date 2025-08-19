#ifndef _QD_H_
#define _QD_H_

#define QD_NO_DISC 		0
#define QD_DISC_READY 	1
#define QD_HEAD_HOME 	2
#define QD_ERR	 		8

extern int qd_init(void);
extern int qd_set_drive_content(char *file_path);
extern void qd_write(uint8_t addr_offset, unsigned char dt);
extern uint8_t qd_read(uint8_t addr_offset);

#endif
