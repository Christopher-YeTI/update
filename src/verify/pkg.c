/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2012-2014 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2013 Bryan Drewery <bdrewery@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ucl.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "config.h"

struct sig_cert {
	char *name;
	unsigned char *sig;
	int siglen;
	unsigned char *cert;
	int certlen;
	bool trusted;
};

struct pubkey {
	unsigned char *sig;
	int siglen;
};

typedef enum {
	HASH_UNKNOWN,
	HASH_SHA256,
} hash_t;

struct fingerprint {
	hash_t type;
	char *name;
	char hash[BUFSIZ];
	STAILQ_ENTRY(fingerprint) next;
};

static int use_quiet = 0;

static char *use_repo = NULL;

STAILQ_HEAD(fingerprint_list, fingerprint);

static struct fingerprint *
parse_fingerprint(ucl_object_t *obj)
{
	const ucl_object_t *cur;
	ucl_object_iter_t it = NULL;
	const char *function, *fp, *key;
	struct fingerprint *f;
	hash_t fct = HASH_UNKNOWN;

	function = fp = NULL;

	while ((cur = ucl_iterate_object(obj, &it, true))) {
		key = ucl_object_key(cur);
		if (cur->type != UCL_STRING)
			continue;
		if (strcasecmp(key, "function") == 0) {
			function = ucl_object_tostring(cur);
			continue;
		}
		if (strcasecmp(key, "fingerprint") == 0) {
			fp = ucl_object_tostring(cur);
			continue;
		}
	}

	if (fp == NULL || function == NULL)
		return (NULL);

	if (strcasecmp(function, "sha256") == 0)
		fct = HASH_SHA256;

	if (fct == HASH_UNKNOWN) {
		warnx("Unsupported hashing function: %s", function);
		return (NULL);
	}

	f = calloc(1, sizeof(struct fingerprint));
	f->type = fct;
	strlcpy(f->hash, fp, sizeof(f->hash));

	return (f);
}

static void
free_fingerprint_list(struct fingerprint_list* list)
{
	struct fingerprint *fingerprint, *tmp;

	STAILQ_FOREACH_SAFE(fingerprint, list, next, tmp) {
		free(fingerprint->name);
		free(fingerprint);
	}
	free(list);
}

static struct fingerprint *
load_fingerprint(const char *dir, const char *filename)
{
	ucl_object_t *obj = NULL;
	struct ucl_parser *p = NULL;
	struct fingerprint *f;
	char path[MAXPATHLEN];

	f = NULL;

	snprintf(path, MAXPATHLEN, "%s/%s", dir, filename);

	p = ucl_parser_new(0);
	if (!ucl_parser_add_file(p, path)) {
		warnx("%s: %s", path, ucl_parser_get_error(p));
		ucl_parser_free(p);
		return (NULL);
	}

	obj = ucl_parser_get_object(p);

	if (obj->type == UCL_OBJECT)
		f = parse_fingerprint(obj);

	if (f != NULL)
		f->name = strdup(filename);

	ucl_object_unref(obj);
	ucl_parser_free(p);

	return (f);
}

static struct fingerprint_list *
load_fingerprints(const char *path, int *count)
{
	DIR *d;
	struct dirent *ent;
	struct fingerprint *finger;
	struct fingerprint_list *fingerprints;

	*count = 0;

	fingerprints = calloc(1, sizeof(struct fingerprint_list));
	if (fingerprints == NULL)
		return (NULL);
	STAILQ_INIT(fingerprints);

	if ((d = opendir(path)) == NULL) {
		free(fingerprints);

		return (NULL);
	}

	while ((ent = readdir(d))) {
		if (strcmp(ent->d_name, ".") == 0 ||
		    strcmp(ent->d_name, "..") == 0)
			continue;
		finger = load_fingerprint(path, ent->d_name);
		if (finger != NULL) {
			STAILQ_INSERT_TAIL(fingerprints, finger, next);
			++(*count);
		}
	}

	closedir(d);

	return (fingerprints);
}

static void
sha256_hash(unsigned char hash[SHA256_DIGEST_LENGTH],
    char out[SHA256_DIGEST_LENGTH * 2 + 1])
{
	int i;

	for (i = 0; i < SHA256_DIGEST_LENGTH; i++)
		sprintf(out + (i * 2), "%02x", hash[i]);

	out[SHA256_DIGEST_LENGTH * 2] = '\0';
}

static void
sha256_buf(char *buf, size_t len, char out[SHA256_DIGEST_LENGTH * 2 + 1])
{
	unsigned char hash[SHA256_DIGEST_LENGTH];
	SHA256_CTX sha256;

	out[0] = '\0';

	SHA256_Init(&sha256);
	SHA256_Update(&sha256, buf, len);
	SHA256_Final(hash, &sha256);
	sha256_hash(hash, out);
}

static int
sha256_fd(int fd, char out[SHA256_DIGEST_LENGTH * 2 + 1])
{
	int my_fd;
	FILE *fp;
	char buffer[BUFSIZ];
	unsigned char hash[SHA256_DIGEST_LENGTH];
	size_t r;
	int ret;
	SHA256_CTX sha256;

	fp = NULL;
	ret = 1;

	out[0] = '\0';

	/* Duplicate the fd so that fclose(3) does not close it. */
	if ((my_fd = dup(fd)) == -1) {
		warnx("dup");
		goto cleanup;
	}

	if ((fp = fdopen(my_fd, "rb")) == NULL) {
		warnx("fdopen");
		goto cleanup;
	}

	SHA256_Init(&sha256);

	while ((r = fread(buffer, 1, BUFSIZ, fp)) > 0)
		SHA256_Update(&sha256, buffer, r);

	if (ferror(fp) != 0) {
		warnx("fread");
		goto cleanup;
	}

	SHA256_Final(hash, &sha256);
	sha256_hash(hash, out);
	ret = 0;

cleanup:
	if (fp != NULL)
		fclose(fp);
	else if (my_fd != -1)
		close(my_fd);
	(void)lseek(fd, 0, SEEK_SET);

	return (ret);
}

static EVP_PKEY *
load_public_key_file(const char *file)
{
	EVP_PKEY *pkey;
	BIO *bp;
	char errbuf[1024];

	bp = BIO_new_file(file, "r");
	if (!bp)
		errx(EXIT_FAILURE, "Unable to read %s", file);

	if ((pkey = PEM_read_bio_PUBKEY(bp, NULL, NULL, NULL)) == NULL)
		warnx("ici: %s", ERR_error_string(ERR_get_error(), errbuf));

	BIO_free(bp);

	return (pkey);
}

static EVP_PKEY *
load_public_key_buf(const unsigned char *cert, int certlen)
{
	EVP_PKEY *pkey;
	BIO *bp;
	char errbuf[1024];

	bp = BIO_new_mem_buf(__DECONST(void *, cert), certlen);

	if ((pkey = PEM_read_bio_PUBKEY(bp, NULL, NULL, NULL)) == NULL)
		warnx("%s", ERR_error_string(ERR_get_error(), errbuf));

	BIO_free(bp);

	return (pkey);
}

static bool
rsa_verify_cert(int fd, const char *sigfile, const unsigned char *key,
    int keylen, unsigned char *sig, int siglen)
{
	EVP_MD_CTX *mdctx;
	EVP_PKEY *pkey;
	char sha256[(SHA256_DIGEST_LENGTH * 2) + 2];
	char errbuf[1024];
	bool ret;

	pkey = NULL;
	mdctx = NULL;
	ret = false;

	SSL_load_error_strings();

	/* Compute SHA256 of the file. */
	if (lseek(fd, 0, 0) == -1) {
		warn("lseek");
		goto cleanup;
	}
	if ((sha256_fd(fd, sha256)) == -1) {
		warnx("Error creating SHA256 hash for file");
		goto cleanup;
	}

	if (sigfile != NULL) {
		if ((pkey = load_public_key_file(sigfile)) == NULL) {
			warnx("Error reading public key");
			goto cleanup;
		}
	} else {
		if ((pkey = load_public_key_buf(key, keylen)) == NULL) {
			warnx("Error reading public key");
			goto cleanup;
		}
	}

	/* Verify signature of the SHA256(pkg) is valid. */
	if ((mdctx = EVP_MD_CTX_create()) == NULL) {
		warnx("%s", ERR_error_string(ERR_get_error(), errbuf));
		goto error;
	}

	if (EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, pkey) != 1) {
		warnx("%s", ERR_error_string(ERR_get_error(), errbuf));
		goto error;
	}
	if (EVP_DigestVerifyUpdate(mdctx, sha256, strlen(sha256)) != 1) {
		warnx("%s", ERR_error_string(ERR_get_error(), errbuf));
		goto error;
	}

	if (EVP_DigestVerifyFinal(mdctx, sig, siglen) != 1) {
		warnx("%s", ERR_error_string(ERR_get_error(), errbuf));
		goto error;
	}

	ret = true;
	if (!use_quiet) {
		printf("done\n");
	}
	goto cleanup;

error:
	if (!use_quiet) {
		printf("failed\n");
	}

cleanup:
	if (pkey)
		EVP_PKEY_free(pkey);
	if (mdctx)
		EVP_MD_CTX_destroy(mdctx);
	ERR_free_strings();

	return (ret);
}

static struct pubkey *
read_pubkey(int fd)
{
	struct pubkey *pk;
	char *sigb;
	size_t sigsz;
	FILE *sig;
	char buf[4096];
	int r;

	if (lseek(fd, 0, 0) == -1) {
		warn("lseek");
		return (NULL);
	}

	sigsz = 0;
	sigb = NULL;
	sig = open_memstream(&sigb, &sigsz);
	if (sig == NULL)
		err(EXIT_FAILURE, "open_memstream()");

	while ((r = read(fd, buf, sizeof(buf))) >0) {
		fwrite(buf, 1, r, sig);
	}

	fclose(sig);
	pk = calloc(1, sizeof(struct pubkey));
	pk->siglen = sigsz;
	pk->sig = calloc(1, pk->siglen);
	memcpy(pk->sig, sigb, pk->siglen);
	free(sigb);

	return (pk);
}

static struct sig_cert *
parse_cert(int fd) {
	int my_fd;
	struct sig_cert *sc;
	FILE *fp, *sigfp, *certfp, *tmpfp;
	char *line;
	char *sig, *cert;
	size_t linecap, sigsz, certsz;
	ssize_t linelen;

	sc = NULL;
	line = NULL;
	linecap = 0;
	sig = cert = NULL;
	sigfp = certfp = tmpfp = NULL;

	if (lseek(fd, 0, 0) == -1) {
		warn("lseek");
		return (NULL);
	}

	/* Duplicate the fd so that fclose(3) does not close it. */
	if ((my_fd = dup(fd)) == -1) {
		warnx("dup");
		return (NULL);
	}

	if ((fp = fdopen(my_fd, "rb")) == NULL) {
		warn("fdopen");
		close(my_fd);
		return (NULL);
	}

	sigsz = certsz = 0;
	sigfp = open_memstream(&sig, &sigsz);
	if (sigfp == NULL)
		err(EXIT_FAILURE, "open_memstream()");
	certfp = open_memstream(&cert, &certsz);
	if (certfp == NULL)
		err(EXIT_FAILURE, "open_memstream()");

	while ((linelen = getline(&line, &linecap, fp)) > 0) {
		if (strcmp(line, "SIGNATURE\n") == 0) {
			tmpfp = sigfp;
			continue;
		} else if (strcmp(line, "CERT\n") == 0) {
			tmpfp = certfp;
			continue;
		} else if (strcmp(line, "END\n") == 0) {
			break;
		}
		if (tmpfp != NULL)
			fwrite(line, 1, linelen, tmpfp);
	}

	fclose(fp);
	fclose(sigfp);
	fclose(certfp);

	sc = calloc(1, sizeof(struct sig_cert));
	sc->siglen = sigsz -1; /* Trim out unrelated trailing newline */
	sc->sig = (unsigned char *)sig;

	sc->certlen = certsz;
	sc->cert = (unsigned char *)cert;

	return (sc);
}

static bool
verify_pubsignature(int fd_pkg, int fd_sig)
{
	struct pubkey *pk;
	const char *pubkey;
	bool ret;

	pk = NULL;
	pubkey = NULL;
	ret = false;
	if (config_string(PUBKEY, &pubkey) != 0) {
		warnx("No CONFIG_PUBKEY defined");
		goto cleanup;
	}

	if ((pk = read_pubkey(fd_sig)) == NULL) {
		warnx("Error reading signature");
		goto cleanup;
	}

	/* Verify the signature. */
	if (!use_quiet) {
		printf("Verifying signature with public key %s... ", pubkey);
	}
	if (rsa_verify_cert(fd_pkg, pubkey, NULL, 0, pk->sig,
	    pk->siglen) == false) {
		fprintf(stderr, "Signature is not valid\n");
		goto cleanup;
	}

	ret = true;

cleanup:
	if (pk) {
		free(pk->sig);
		free(pk);
	}

	return (ret);
}

static bool
verify_signature(int fd_pkg, int fd_sig)
{
	struct fingerprint_list *trusted, *revoked;
	struct fingerprint *fingerprint;
	struct sig_cert *sc;
	bool ret;
	int trusted_count, revoked_count;
	const char *fingerprints;
	char path[MAXPATHLEN];
	char hash[SHA256_DIGEST_LENGTH * 2 + 1];

	sc = NULL;
	trusted = revoked = NULL;
	ret = false;

	/* Read and parse fingerprints. */
	if (config_string(FINGERPRINTS, &fingerprints) != 0) {
		warnx("No CONFIG_FINGERPRINTS defined");
		goto cleanup;
	}

	snprintf(path, MAXPATHLEN, "%s/trusted", fingerprints);
	if ((trusted = load_fingerprints(path, &trusted_count)) == NULL) {
		warnx("Error loading trusted certificates");
		goto cleanup;
	}

	if (trusted_count == 0 || trusted == NULL) {
		fprintf(stderr, "No trusted certificates found.\n");
		goto cleanup;
	}

	snprintf(path, MAXPATHLEN, "%s/revoked", fingerprints);
	if ((revoked = load_fingerprints(path, &revoked_count)) == NULL) {
		warnx("Error loading revoked certificates");
		goto cleanup;
	}

	/* Read certificate and signature in. */
	if ((sc = parse_cert(fd_sig)) == NULL) {
		warnx("Error parsing certificate");
		goto cleanup;
	}
	/* Explicitly mark as non-trusted until proven otherwise. */
	sc->trusted = false;

	/* Parse signature and pubkey out of the certificate */
	sha256_buf((char *)sc->cert, sc->certlen, hash);

	/* Check if this hash is revoked */
	if (revoked != NULL) {
		STAILQ_FOREACH(fingerprint, revoked, next) {
			if (strcasecmp(fingerprint->hash, hash) == 0) {
				fprintf(stderr, "The file was signed with "
				    "revoked certificate %s\n",
				    fingerprint->name);
				goto cleanup;
			}
		}
	}

	STAILQ_FOREACH(fingerprint, trusted, next) {
		if (strcasecmp(fingerprint->hash, hash) == 0) {
			sc->trusted = true;
			sc->name = strdup(fingerprint->name);
			break;
		}
	}

	if (sc->trusted == false) {
		fprintf(stderr, "No trusted fingerprint found matching "
		    "file's certificate\n");
		goto cleanup;
	}

	/* Verify the signature. */
	if (!use_quiet) {
		printf("Verifying signature with trusted certificate %s... ",
		    sc->name);
	}
	if (rsa_verify_cert(fd_pkg, NULL, sc->cert, sc->certlen, sc->sig,
	    sc->siglen) == false) {
		fprintf(stderr, "Signature is not valid\n");
		goto cleanup;
	}

	ret = true;

cleanup:
	if (trusted)
		free_fingerprint_list(trusted);
	if (revoked)
		free_fingerprint_list(revoked);
	if (sc) {
		free(sc->cert);
		free(sc->sig);
		free(sc->name);
		free(sc);
	}

	return (ret);
}

static int
verify_local(const char *pkgpath)
{
	char path[MAXPATHLEN];
	char pkgstatic[MAXPATHLEN];
	const char *signature_type;
	int fd_pkg, fd_sig, ret;

	fd_sig = -1;
	ret = -1;

	fd_pkg = open(pkgpath, O_RDONLY);
	if (fd_pkg == -1)
		err(EXIT_FAILURE, "Unable to open %s", pkgpath);

	if (config_string(SIGNATURE_TYPE, &signature_type) != 0) {
		warnx("Error looking up SIGNATURE_TYPE");
		goto cleanup;
	}
	if (signature_type != NULL &&
	    strcasecmp(signature_type, "NONE") != 0) {
		if (strcasecmp(signature_type, "FINGERPRINTS") == 0) {

			snprintf(path, sizeof(path), "%s.sig", pkgpath);

			if ((fd_sig = open(path, O_RDONLY)) == -1) {
				fprintf(stderr, "Signature for file not "
				    "available.\n");
				goto cleanup;
			}

			if (verify_signature(fd_pkg, fd_sig) == false)
				goto cleanup;

		} else if (strcasecmp(signature_type, "PUBKEY") == 0) {

			snprintf(path, sizeof(path), "%s.pubkeysig", pkgpath);

			if ((fd_sig = open(path, O_RDONLY)) == -1) {
				fprintf(stderr, "Signature for file not "
				    "available.\n");
				goto cleanup;
			}

			if (verify_pubsignature(fd_pkg, fd_sig) == false)
				goto cleanup;

		} else {
			warnx("Signature type %s is not supported for "
			    "verification.", signature_type);
			goto cleanup;
		}
	} else if (signature_type == NULL) {
		warnx("Signature type disabled is not supported for "
		    "verification.");
		goto cleanup;
	}

	/* all ok */
	ret = 0;

cleanup:
	close(fd_pkg);
	if (fd_sig != -1)
		close(fd_sig);

	return (ret);
}

static void
usage(void)
{
	fprintf(stderr, "Usage: man yetisense-verify\n");
	exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
	char *filepath;
	int c;

	while ((c = getopt(argc, argv, "alqr:")) != -1) {
		switch (c) {
		case 'a': {
			char abi[BUFSIZ];

			if (pkg_get_myabi(abi, sizeof(abi)) != 0) {
				errx(EXIT_FAILURE, "Failed to determine "
				    "the system ABI");
			}
			printf("%s\n", abi);

			return (EXIT_SUCCESS);
		}
		case 'l':
			config_init(NULL);
			config_print_repos();

			return (EXIT_SUCCESS);
		case 'q':
			use_quiet = 1;
			break;
		case 'r':
			use_repo = strdup(optarg);
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1 || (filepath = argv[0]) == NULL) {
		usage();
		/* NOTREACHED */
	}

	if (!use_repo) {
		use_repo = strdup("YETIsense");
	}

	config_init(use_repo);

	if (config_count_repos() != 1) {
		fprintf(stderr, "Repository not found: %s\n", use_repo);
		exit(EXIT_FAILURE);
	}

	if (verify_local(filepath) != 0) {
		exit(EXIT_FAILURE);
	}

	config_finish();

	free(use_repo);

	return (EXIT_SUCCESS);
}
