PROG=	yetisense-verify
MAN=	yetisense-verify.8

SRCS=	config.c pkg.c

CFLAGS+=-I/usr/local/include
LDADD+=	-lutil -lssl -lcrypto
LDADD+=	-L/usr/local/lib -lucl

LOCALBASE?=${DESTDIR}/usr/local
PREFIX?=${LOCALBASE}

.include <bsd.prog.mk>
