# $FreeBSD$

PROG=	mount_fusefs
SRCS=	mount_fusefs.c getmntopts.c
MAN8=	mount_fusefs.8
NO_MANCOMPRESS?= yes

BINDIR?= /usr/sbin

# mount_fusefs needs mntopts.h and getmntopts.c from src/sbin/mount/
MOUNT?=	/usr/src/sbin/mount
CFLAGS+= -I${MOUNT} -I${.CURDIR}/../fuse
.PATH: ${MOUNT}

.include <bsd.prog.mk>
