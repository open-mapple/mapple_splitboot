// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Pritish Joshi (DrtSinX98)
/*
 * mapple-install's dual boot verbs (built in with -DMAPPLE_SPLIT; the core is build/mapple's mapple-install.h):
 *
 *   mapple-install split-carve DISK [--size GIB] [--check]
 *	xnusys (GIB, default 22) carved off the end of userdata; userdata's start, metadata, xnusys's first MiB and
 *	its saved area's header zeroed. Boot is not touched: the phone stays Android until split-chain.
 *   mapple-install split-chain DISK BOOT LOADER PAYLOAD
 *	after the root is flashed (and checked with verify-root): boot kept in xnusys's saved area unless it carries
 *	the loader already (an Android update's boot is kept anew), then chained, and the chained image recorded
 *   mapple-install split-uninstall DISK BOOT [--check]
 *	boot back from the saved area (only while it is still the image chained here), the GPT as before the carve,
 *	userdata's start and metadata zeroed
 *
 * DISK is the whole unit the partitions are on (/dev/block/sdc on spaced, a file in a dry run), BOOT the boot
 * partition on it. After the first write that matters, a failure puts back what was written (from copies in
 * memory, in an atexit handler) before the program ends.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>
#ifdef __linux__
#include <dirent.h>
#include <linux/blkpg.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#else
enum { BLKPG_ADD_PARTITION = 1, BLKPG_DEL_PARTITION, BLKPG_RESIZE_PARTITION };	/* a dry run has no kernel view */
#endif
#ifdef __APPLE__
#include <sys/random.h>
#endif

#include "mapple-install.h"

#define BS		4096u
#define ZERO_DATA	(64 * MiB)

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* --- restore journal: what was overwritten, put back in reverse when anything fails ----------------------------- */

struct undo { int fd; uint64_t off; const uint8_t *buf; size_t n; const char *what; };
static struct undo journal[4];
static int journal_n;
struct pg_undo { int fd, op, pno; uint64_t first, last; };
static struct pg_undo pg_journal[4];
static int pg_n;

static void remember(int fd, uint64_t off, const uint8_t *buf, size_t n, const char *what)
{
	journal[journal_n++] = (struct undo){ fd, off, buf, n, what };
}

static void forget(void)
{
	journal_n = pg_n = 0;
}

static int blkpg(int fd, int op, uint64_t first, uint64_t last, int pno)
{
#ifdef __linux__
	struct blkpg_partition p = { .start = (long long)(first * BS), .length = (long long)((last - first + 1) * BS),
				     .pno = pno };
	struct blkpg_ioctl_arg a = { .op = op, .datalen = sizeof(p), .data = &p };

	return ioctl(fd, BLKPG, &a);
#else
	(void)fd, (void)op, (void)first, (void)last, (void)pno;
	return 0;
#endif
}

static void restore(void)
{
	int bad = 0;

	if (!journal_n)
		return;
	fprintf(stderr, "mapple-install: putting back what was written\n");
	while (journal_n > 0) {
		struct undo *u = &journal[--journal_n];
		bad |= write_back_try(u->fd, u->what, u->buf, u->n, u->off);
	}
	while (pg_n > 0) {
		struct pg_undo *p = &pg_journal[--pg_n];
		blkpg(p->fd, p->op, p->first, p->last, p->pno);
	}
	fprintf(stderr, bad ? "mapple-install: NOT everything could be put back: do not reboot\n"
			    : "mapple-install: put back as it was (zeroed data stays zeroed: Android sets itself up again)\n");
}

/* the running kernel's view, so that the recovery cannot format the old extents; a failure is only reported */
static void pg(int fd, int op, uint64_t first, uint64_t last, int pno, int undo_op, uint64_t ufirst, uint64_t ulast)
{
	if (blkpg(fd, op, first, last, pno)) {
		printf("BLKPG on partition %d: %s. THE RECOVERY STILL SEES THE OLD PARTITIONS: format nothing, reboot "
		       "when this is done\n", pno, strerror(errno));
		return;
	}
	pg_journal[pg_n++] = (struct pg_undo){ fd, undo_op, pno, ufirst, ulast };
}

/* nothing may hold a partition that is zeroed or resized: no mount, no device-mapper on it */
static void unheld(const char *disk, int pno)
{
#ifdef __linux__
	const char *base = strrchr(disk, '/') ? strrchr(disk, '/') + 1 : disk;
	char path[256], node[64], line[512];
	DIR *d;
	FILE *m;
	struct dirent *e;

	snprintf(node, sizeof(node), "%s%d", base, pno);
	snprintf(path, sizeof(path), "/sys/class/block/%s/holders", node);
	if ((d = opendir(path))) {
		while ((e = readdir(d)))
			if (e->d_name[0] != '.')
				die("%s is held by %s: unmount it (reboot the recovery if that does not free it)", node, e->d_name);
		closedir(d);
	}
	if ((m = fopen("/proc/mounts", "r"))) {
		size_t k = strlen(node);
		while (fgets(line, sizeof(line), m)) {
			const char *dev = strncmp(line, "/dev/block/", 11) ? line : line + 11;
			if (!strncmp(dev, node, k) && dev[k] == ' ')
				die("%s is mounted: %s", node, line);
		}
		fclose(m);
	}
#else
	(void)disk, (void)pno;
#endif
}

static void zero(int fd, uint64_t off, uint64_t n, const char *what)
{
	static uint8_t *z;

	if (!z && !(z = calloc(1, ZERO_DATA)))
		die("out of memory");
	if (n > ZERO_DATA)
		die("%s: %llu bytes to zero", what, (unsigned long long)n);
	write_back(fd, what, z, (size_t)n, off);
}

/* --- GPT ------------------------------------------------------------------------------------------------------- */

struct gpt {
	uint8_t *head, *tail;
	size_t head_n, tail_n;
	uint64_t blocks;
	uint32_t nent, esz, hsize, entlba;
};

static const uint8_t linux_type[16] = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
					0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };

static uint32_t gcrc(const uint8_t *p, size_t n) { return (uint32_t)crc32(0, p, (uInt)n); }
static uint8_t *gpt_entries(const struct gpt *g, int primary) { return primary ? g->head + (size_t)g->entlba * BS : g->tail; }
static uint8_t *gpt_header(const struct gpt *g, int primary) { return primary ? g->head + BS : g->tail + g->tail_n - BS; }

static int gpt_find(const struct gpt *g, const char *name, uint64_t *first, uint64_t *last)
{
	const uint8_t *e = gpt_entries(g, 1);

	for (uint32_t i = 0; i < g->nent; i++, e += g->esz) {
		int c = 0;
		while (name[c] && e[56 + 2 * c] == (uint8_t)name[c] && !e[57 + 2 * c])
			c++;
		if (!name[c] && !e[56 + 2 * c] && !e[57 + 2 * c]) {
			*first = le64(e + 32);
			*last = le64(e + 40);
			return (int)i;
		}
	}
	return -1;
}

/* one copy checked as a firmware would: signature, header CRC, its own and the other's place, entries CRC */
static void gpt_check_copy(const struct gpt *g, int primary, const char *label)
{
	const uint8_t *h = gpt_header(g, primary);
	uint8_t raw[512];
	uint64_t my = primary ? 1 : g->blocks - 1, alt = primary ? g->blocks - 1 : 1;

	if (memcmp(h, "EFI PART", 8) || le32(h + 12) < 92 || le32(h + 12) > sizeof(raw))
		die("%s: no GPT header", label);
	memcpy(raw, h, le32(h + 12));
	put32(raw + 16, 0);
	if (gcrc(raw, le32(h + 12)) != le32(h + 16))
		die("%s: the header does not match its CRC", label);
	if (le64(h + 24) != my || le64(h + 32) != alt)
		die("%s: header at %llu says %llu/%llu, the unit has %llu blocks", label, (unsigned long long)my,
		    (unsigned long long)le64(h + 24), (unsigned long long)le64(h + 32), (unsigned long long)g->blocks);
	if (gcrc(gpt_entries(g, primary), (size_t)g->nent * g->esz) != le32(h + 88))
		die("%s: the entries do not match their CRC", label);
}

/* both copies: the primary through its entries, the backup from its entries (spaced leaves 3 blocks between them
 * and its header) to the last block */
static void gpt_read(int fd, uint64_t blocks, struct gpt *g, const char *label)
{
	uint8_t h[BS];
	uint32_t eblocks;
	uint64_t tail_entlba;

	memset(g, 0, sizeof(*g));
	g->blocks = blocks;
	pread_all(fd, h, BS, BS);
	if (memcmp(h, "EFI PART", 8))
		die("%s: no GPT header at block 1", label);
	g->hsize = le32(h + 12);
	g->entlba = (uint32_t)le64(h + 72);
	g->nent = le32(h + 80);
	g->esz = le32(h + 84);
	if (g->esz != 128 || !g->nent || g->nent > 1024 || g->entlba < 2 || g->entlba > 64)
		die("%s: GPT with %u entries of %u bytes at %u", label, g->nent, g->esz, g->entlba);
	eblocks = (g->nent * g->esz + BS - 1) / BS;
	g->head_n = (size_t)(g->entlba + eblocks) * BS;
	pread_all(fd, h, BS, (blocks - 1) * BS);
	tail_entlba = le64(h + 72);
	if (memcmp(h, "EFI PART", 8) || tail_entlba + eblocks > blocks - 1 || tail_entlba + 64 < blocks)
		die("%s: no backup GPT header in the last block, or its entries not near it", label);
	g->tail_n = (size_t)(blocks - tail_entlba) * BS;
	if (!(g->head = malloc(g->head_n)) || !(g->tail = malloc(g->tail_n)))
		die("out of memory");
	pread_all(fd, g->head, g->head_n, 0);
	pread_all(fd, g->tail, g->tail_n, blocks * BS - g->tail_n);
	gpt_check_copy(g, 1, label);
	gpt_check_copy(g, 0, label);
	if (memcmp(gpt_entries(g, 1), gpt_entries(g, 0), (size_t)g->nent * g->esz))
		die("%s: the two copies' entries differ", label);
}

static struct gpt gpt_copy(const struct gpt *g)
{
	struct gpt c = *g;

	if (!(c.head = malloc(g->head_n)) || !(c.tail = malloc(g->tail_n)))
		die("out of memory");
	memcpy(c.head, g->head, g->head_n);
	memcpy(c.tail, g->tail, g->tail_n);
	return c;
}

/* after the primary's entries changed: the backup's entries, both CRCs */
static void gpt_seal(struct gpt *g)
{
	size_t n = (size_t)g->nent * g->esz;

	memcpy(gpt_entries(g, 0), gpt_entries(g, 1), n);
	for (int p = 0; p < 2; p++) {
		uint8_t *h = gpt_header(g, p);
		put32(h + 88, gcrc(gpt_entries(g, p), n));
		put32(h + 16, 0);
		put32(h + 16, gcrc(h, g->hsize));
	}
}

/* NOW written over WAS (backup copy first), each put back if anything fails later; then read again and checked */
static void gpt_write(int fd, const struct gpt *now, const struct gpt *was, const char *label)
{
	struct gpt check;

	remember(fd, now->blocks * BS - was->tail_n, was->tail, was->tail_n, "GPT backup copy (put back)");
	remember(fd, 0, was->head, was->head_n, "GPT primary copy (put back)");
	write_back(fd, "GPT backup copy", now->tail, now->tail_n, now->blocks * BS - now->tail_n);
	write_back(fd, "GPT primary copy", now->head, now->head_n, 0);
	gpt_read(fd, now->blocks, &check, label);
	if (memcmp(check.head, now->head, now->head_n) || memcmp(check.tail, now->tail, now->tail_n))
		die("%s: not what was written", label);
}

/* --- the rest ---------------------------------------------------------------------------------------------- */

static int open_disk(const char *path, uint64_t *blocks)
{
	int fd = open_dev(path, O_RDWR);
	uint64_t n = size_of(fd);

#ifdef __linux__
	int ss = 0;
	if (!ioctl(fd, BLKSSZGET, &ss) && ss != (int)BS)
		die("%s: %d-byte blocks, not %u", path, ss, BS);
	ioctl(fd, BLKFLSBUF, 0);
#endif
	if (!n || n % BS)
		die("%s: %llu bytes", path, (unsigned long long)n);
	*blocks = n / BS;
	return fd;
}

/* BOOT read whole; it must be DISK's boot extent (the same partition) */
static uint8_t *load_boot(int disk, const struct gpt *g, const char *path, size_t *n)
{
	uint64_t first, last;
	uint8_t *b, *d;

	if (gpt_find(g, "boot", &first, &last) < 0)
		die("no boot partition in the GPT");
	b = load(path, n);
	if (*n != (last - first + 1) * BS)
		die("%s: %zu bytes, the GPT's boot %llu", path, *n, (unsigned long long)((last - first + 1) * BS));
	if (!(d = malloc(*n)))
		die("out of memory");
	pread_all(disk, d, *n, first * BS);
	if (memcmp(b, d, *n))
		die("%s is not the boot partition of this disk", path);
	free(d);
	return b;
}

/* whether a boot image's kernel carries a loader in front of the Android Image (chain()'s res2 at 0x28) */
static int chained(const uint8_t *b, size_t n)
{
	uint8_t img[0x40];
	uint32_t page, ksize;
	z_stream z = { 0 };

	if (n < 2048 || memcmp(b, "ANDROID!", 8) || le32(b + 40) != 2)
		die("the boot partition holds no Android boot image (header v2)");
	page = le32(b + 36);
	ksize = le32(b + 8);
	if (page < 2048 || page + (uint64_t)ksize > n)
		die("the boot image's kernel runs past its end");
	if (b[page] != 0x1f || b[page + 1] != 0x8b)
		return 0;
	if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK)
		die("inflate");
	z.next_in = (Bytef *)(b + page);
	z.avail_in = ksize;
	z.next_out = img;
	z.avail_out = sizeof(img);
	inflate(&z, Z_SYNC_FLUSH);
	inflateEnd(&z);
	if (z.avail_out || memcmp(img + 0x38, "ARM\x64", 4))
		die("the boot image's kernel is not an arm64 Image");
	return le64(img + 0x28) != 0;
}

/* one item of the saved area (mapple-install.h's layout): its size and SHA-256; 0 if the header or item is not there */
static int saved_item(int fd, uint64_t end, const char *name, uint64_t *size, uint8_t sha[32])
{
	uint8_t head[4096], digest[32];
	uint32_t count;

	pread_all(fd, head, sizeof(head), end - AREAS);
	sha256(head, 4064, digest);
	count = le32(head + 20);
	if (memcmp(head, "mapple-saved\0\0\0\0", 16) || le32(head + 16) != 1 || count > 62 || memcmp(head + 4064, digest, 32))
		return 0;
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *it = head + 64 + 64 * i;
		if (!strncmp((const char *)it, name, 16)) {
			*size = le64(it + 16);
			memcpy(sha, it + 32, 32);
			return 1;
		}
	}
	return 0;
}

static void opts(int argc, char **argv, int from, double *gib, int *check)
{
	for (int i = from; i < argc; i++) {
		if (!strcmp(argv[i], "--check") && check)
			*check = 1;
		else if (!strcmp(argv[i], "--size") && gib && i + 1 < argc)
			*gib = atof(argv[++i]);
		else
			die("unknown option %s", argv[i]);
	}
}

static void show(const char *name, uint64_t first, uint64_t last)
{
	printf("  %-10s %10llu..%-10llu %8.2f GiB\n", name, (unsigned long long)first, (unsigned long long)last,
	       (double)(last - first + 1) * BS / (1u << 30));
}

static int split_carve(int argc, char **argv)
{
	const char *diskp = argv[1];
	double gib = 22;
	int check = 0, disk, iu, im, slot = -1;
	uint64_t blocks, us, ue, ms, me, xs, xe, f, l, align = MiB / BS, sz;
	uint8_t *e, *s;
	struct gpt g, was;
	static const uint8_t blank[128];

	opts(argc, argv, 2, &gib, &check);
	disk = open_disk(diskp, &blocks);
	gpt_read(disk, blocks, &g, diskp);
	if (gpt_find(&g, "xnusys", &f, &l) >= 0)
		die("xnusys is in the GPT already (carved before: flash the root, or the uninstall zip)");
	if ((iu = gpt_find(&g, "userdata", &us, &ue)) < 0 || (im = gpt_find(&g, "metadata", &ms, &me)) < 0)
		die("no userdata or no metadata partition");
	e = gpt_entries(&g, 1);
	for (uint32_t i = 0; i < g.nent && slot < 0; i++)
		if (!memcmp(e + (size_t)i * g.esz, blank, g.esz))
			slot = (int)i;
	if (slot < 0)
		die("no free (all-zero) GPT entry");

	/* the end of userdata, its start on a 1 MiB boundary (gpt-carve.py's rule) */
	sz = (uint64_t)(gib * (1u << 30)) / BS;
	xe = ue;
	xs = (xe + 1 - sz) / align * align;
	if (gib < 4 || xs <= us || (xs - us) * BS < (8ull << 30))
		die("userdata (%.2f GiB) cannot give %.2f GiB and keep 8 GiB", (double)(ue - us + 1) * BS / (1u << 30), gib);
	printf("split-carve: %llu blocks of %u; now\n", (unsigned long long)blocks, BS);
	show("userdata", us, ue);
	printf("after\n");
	show("userdata", us, xs - 1);
	show("xnusys", xs, xe);
	if (check)
		return 0;
	unheld(diskp, iu + 1);
	unheld(diskp, im + 1);

	was = gpt_copy(&g);
	put64(e + (size_t)iu * g.esz + 40, xs - 1);
	s = e + (size_t)slot * g.esz;
	memcpy(s, linux_type, 16);
	if (getentropy(s + 16, 16))
		die("getentropy: %s", strerror(errno));
	s[16 + 7] = (s[16 + 7] & 0x0f) | 0x40;	/* a version 4 GUID */
	s[16 + 8] = (s[16 + 8] & 0x3f) | 0x80;
	put64(s + 32, xs);
	put64(s + 40, xe);
	for (int c = 0; "xnusys"[c]; c++)
		s[56 + 2 * c] = (uint8_t)"xnusys"[c];
	gpt_seal(&g);
	gpt_write(disk, &g, &was, "the new GPT");
	pg(disk, BLKPG_RESIZE_PARTITION, us, xs - 1, iu + 1, BLKPG_RESIZE_PARTITION, us, ue);
	pg(disk, BLKPG_ADD_PARTITION, xs, xe, slot + 1, BLKPG_DEL_PARTITION, xs, xe);

	zero(disk, xs * BS, MiB, "xnusys's first MiB");
	zero(disk, (xe + 1) * BS - AREAS, MiB, "xnusys's saved header");
	zero(disk, us * BS, ZERO_DATA, "userdata's start");
	zero(disk, ms * BS, (me - ms + 1) * BS, "metadata");
	forget();
	printf("split-carve: done. Android starts as before (and sets userdata up again). Next: reboot to the "
	       "bootloader, fastboot flash xnusys <root image>, fastboot reboot recovery, this zip again.\n");
	return 0;
}

static int split_chain(int argc, char **argv)
{
	const char *diskp = argv[1], *bootp = argv[2];
	char *args[3] = { argv[2], argv[3], argv[4] };
	int disk, bootfd;
	uint64_t blocks, xs, xe, end;
	size_t boot_n, n;
	uint8_t *boot, *now;
	struct gpt g;

	opts(argc, argv, 5, NULL, NULL);
	disk = open_disk(diskp, &blocks);
	gpt_read(disk, blocks, &g, diskp);
	if (gpt_find(&g, "xnusys", &xs, &xe) < 0)
		die("no xnusys: carve first");
	end = (xe + 1) * BS;
	boot = load_boot(disk, &g, bootp, &boot_n);
	if (!chained(boot, boot_n)) {
		printf("split-chain: boot carries no loader (first install, or an Android update): kept, then chained\n");
		saved_put(disk, end, "boot", boot, boot_n);
	} else {
		printf("split-chain: boot carries the loader: chained again from its Android Image\n");
	}
	bootfd = open_dev(bootp, O_RDWR);
	remember(bootfd, 0, boot, boot_n, "boot (put back)");
	chain(3, args);
	now = load(bootp, &n);
	saved_put(disk, end, "chained", now, n);
	forget();
	printf("split-chain: done\n");
	return 0;
}

static int split_uninstall(int argc, char **argv)
{
	const char *diskp = argv[1], *bootp = argv[2];
	int check = 0, disk, bootfd, ix, iu, im, ours;
	uint64_t blocks, xs, xe, us, ue, ms, me, f, l, end, size, csize;
	size_t boot_n;
	uint8_t *boot, *saved = NULL, sha[32], csha[32], now[32];
	struct gpt g, was;

	opts(argc, argv, 3, NULL, &check);
	disk = open_disk(diskp, &blocks);
	gpt_read(disk, blocks, &g, diskp);
	ix = gpt_find(&g, "xnusys", &xs, &xe);
	iu = gpt_find(&g, "userdata", &us, &ue);
	im = gpt_find(&g, "metadata", &ms, &me);
	if (ix < 0 || iu < 0 || im < 0)
		die("xnusys, userdata and metadata must all be in the GPT");
	if (ue + 1 != xs || memcmp(gpt_entries(&g, 1) + (size_t)ix * g.esz, linux_type, 16))
		die("xnusys does not follow userdata, or is not the carve's: nothing done");
	if (gpt_find(&g, "darwin", &f, &l) >= 0)	/* the development layout before the merge: not ours to guess */
		die("a darwin partition is there too (an older layout): nothing done");
	end = (xe + 1) * BS;

	/* boot: back only while it is the image chained here; a boot without the loader stays */
	boot = load_boot(disk, &g, bootp, &boot_n);
	ours = chained(boot, boot_n);
	if (ours) {
		sha256(boot, boot_n, now);
		if (!saved_item(disk, end, "chained", &csize, csha) || csize != boot_n || memcmp(csha, now, 32) ||
		    !saved_item(disk, end, "boot", &size, sha) || size != boot_n)
			die("boot carries the loader, but not the image chained here: flash the stock boot image first");
		if (!(saved = malloc(boot_n)))
			die("out of memory");
		pread_all(disk, saved, boot_n, end - AREAS + MiB);
		sha256(saved, boot_n, now);
		if (memcmp(now, sha, 32))
			die("the saved boot does not match its SHA-256: flash the stock boot image first");
	}

	/* the GPT as before the carve: userdata to xnusys's end, xnusys's entry all zero */
	was = gpt_copy(&g);
	put64(gpt_entries(&g, 1) + (size_t)iu * g.esz + 40, xe);
	memset(gpt_entries(&g, 1) + (size_t)ix * g.esz, 0, g.esz);
	gpt_seal(&g);
	printf("split-uninstall: boot %s; after\n", ours ? "back to the saved one" : "has no loader: left as it is");
	show("userdata", us, xe);
	if (check)
		return 0;
	unheld(diskp, iu + 1);
	unheld(diskp, im + 1);
	unheld(diskp, ix + 1);

	if (ours) {
		bootfd = open_dev(bootp, O_RDWR);
		remember(bootfd, 0, boot, boot_n, "boot (put back)");
		write_back(bootfd, "boot", saved, boot_n, 0);
	}
	zero(disk, ms * BS, (me - ms + 1) * BS, "metadata");
	gpt_write(disk, &g, &was, "the GPT before the carve");
	pg(disk, BLKPG_DEL_PARTITION, xs, xe, ix + 1, BLKPG_ADD_PARTITION, xs, xe);
	pg(disk, BLKPG_RESIZE_PARTITION, us, xe, iu + 1, BLKPG_RESIZE_PARTITION, us, ue);
	zero(disk, us * BS, ZERO_DATA, "userdata's start");
	forget();
	printf("split-uninstall: done. Android formats userdata on its next start.\n");
	return 0;
}

int split_main(int argc, char **argv)
{
	atexit(restore);
	if (argc >= 2 && !strcmp(argv[0], "split-carve"))
		return split_carve(argc, argv);
	if (argc == 5 && !strcmp(argv[0], "split-chain"))
		return split_chain(argc, argv);
	if (argc >= 3 && !strcmp(argv[0], "split-uninstall"))
		return split_uninstall(argc, argv);
	fprintf(stderr, "usage: mapple-install split-carve DISK [--size GIB] [--check]\n"
			"       mapple-install split-chain DISK BOOT LOADER PAYLOAD\n"
			"       mapple-install split-uninstall DISK BOOT [--check]\n");
	return 2;
}
