
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>


#define uchar unsigned char

#define FLV_TYPE_AUDIO 0x08
#define FLV_TYPE_VIDEO 0x09
#define FLV_TYPE_META 0x12

void die(char *str)
{
    printf(str);
    exit(1);
}

void usage(void)
{
    printf("Usage:\n");
    printf("  flv_fix_seek  begin.flv   seek.flv   out.flv\n");
    printf("\n");
    printf("  Make a readable file with begin's header and seek's content.\n");
    printf("\n");
    printf("  For example, there is this big flash video out there,\n");
    printf("  but you only want a part of it. So you'd rather:\n");
    printf("    - seek directly to the interesting part in the player,\n");
    printf("    - get the downloaded data from the browser\n");
    printf("      (hard link the file from the browser's cache)\n");
    printf("  and be done with it. Problem with that is the resulting file sometimes\n");
    printf("  is unreadable: it's got just the middle data and without the header's\n");
    printf("  metadata the player can't read it.\n");
    printf("\n");
    printf("  However if you have a little bit of the beginning (begin.flv),\n");    
    printf("  the broken part that won't play (seek.flv) and feed that to\n");
    printf("  fix_flv_seek, it should be enough to make it readable.\n");
    printf("\n");
    exit(1);
}

#define ASSERT(check, format, args...)  do  {	\
	if (!(check))				\
	{ printf(format, ##args); exit(1); }		\
    } while(0)

int my_open(const char *fname, int flags, mode_t mode)
{
    int ret = open(fname, flags, mode);
    if (ret == -1)
	{
	    perror(fname);
	    exit(1);
	}
    return ret;
}

void *my_mmap(void *addr, size_t length, int prot, int flags,
	      int fd, off_t offset)
{
    void *ret = mmap(addr, length, prot, flags, fd, offset);
    if (ret == MAP_FAILED)
	{
	    perror("mmap: ");
	    exit(1);
	}
    return ret;    
}

int get_file_len(int fd)
{
    struct stat st;
    if (fstat(fd, &st))
    {
	perror("fstat: ");
	exit(1);
    }
    return st.st_size;
}

int file_exists(const char *name)
{
    struct stat st;
    if (stat(name, &st) == -1 &&
	errno == ENOENT)
	return 0;
    return 1;
}


int get_len(const uchar *pt, int bytes)
{
    int i, len = 0;
    for (i = 0; i < bytes; i++)
    {
	len = len << 8;	
	len |= pt[i];
    }
    return len;
}

const uchar *skip_tag(const uchar *tag_begin, int body_len)
{
    return tag_begin + body_len + 15;
}

void parse_tag(const uchar *pt, uchar *type, int *body_len,
	       const uchar *beg)
{
    // FIXME: should check we're within the file boundaries
#define OFFSET	(pt - beg)
    int prev_len = 0;
    *type = *pt;
    ASSERT(*type == FLV_TYPE_AUDIO ||
	   *type == FLV_TYPE_VIDEO ||
	   *type == FLV_TYPE_META,
	   "Invalid tag type %#02x at offset %i", *type, OFFSET);

    *body_len = get_len(pt + 1, 3);

    printf("%#08x: Found TAG type %#02x, len %i\n", OFFSET, *type, *body_len);

    /* Check end tag len */
    pt = skip_tag(pt, *body_len);
    
    pt -= 4;
    prev_len = get_len(pt, 4);
    ASSERT(prev_len + 4 == *body_len + 15,
	   "Invalid tag, end of tag length mismatch (%i != %i)\n",
	   prev_len + 4, *body_len + 15);
}

uchar		*head_beg = 0;
const char	*head_fname = 0;
int		head_fd = 0;
int		head_len = 0;

uchar		*broken_beg = 0;
const char	*broken_fname = 0;
int		broken_fd = 0;
int		broken_len = 0;

const char	*out_fname = 0;
int		out_fd = 0;

ssize_t my_write(int fd, const void *buf, size_t count)
{
    int total = 0;
    while (total != count)
    {
	int ret = write(fd, buf, count);
	if (ret == -1)
	{
	    perror(out_fname);
	    exit(1);
	}
	total += ret;
    }
    return total;
}

const uchar* check_head()
{
    const uchar const *beg = head_beg;
    const uchar *pt = beg;
    uchar type;
    int i, len;
    ASSERT(!strncmp((char*)pt, "FLV", 3), "file %s: invalid FLV header\n", head_fname);
    pt += 13;
    
    ASSERT(*pt == FLV_TYPE_META, "Non metadata tag (%#02x) at offset 13\n", *pt);    
    parse_tag(pt, &type, &len, beg);
    pt = skip_tag(pt, len);

    ASSERT(*pt == FLV_TYPE_AUDIO ||
	   *pt == FLV_TYPE_VIDEO,
	   "hum, second tag neither audio or video ...\n");

    // need first two tags for some reason ... investigate
    for (i = 0; i < 2; i++)
    {
	parse_tag(pt, &type, &len, beg);
//	printf("TAG type %#02x, len %i\n", type, len);
	pt = skip_tag(pt, len);	
    }

    return pt;
}

const uchar* check_broken()
{
    const uchar const *beg = broken_beg;
    const uchar *pt = beg;
    uchar type;
    int len;
    ASSERT(!strncmp((char*)pt, "FLV", 3), "file %s: invalid FLV header\n", broken_fname);
    pt += 13;

    while (*pt != FLV_TYPE_VIDEO)
    {
	parse_tag(pt, &type, &len, beg);
	pt = skip_tag(pt, len);

	ASSERT(*pt == FLV_TYPE_AUDIO ||
	       *pt == FLV_TYPE_VIDEO,
	       "hum, second tag neither audio or video ...\n");
    }

    printf("%#08x: First video tag\n", pt - beg);

    return pt;
}

void doit()
{
    const uchar *head_pt;
    const uchar *broken_pt;    
    
    printf("%s: Checking header\n", head_fname);
    head_pt = check_head();
    printf("\n");

    printf("%s: Looking for first video tag\n", broken_fname);    
    broken_pt = check_broken();
    printf("\n");

    out_fd = my_open(out_fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);    
    printf("Writing %s\n", out_fname);
    my_write(out_fd, head_beg, head_pt - head_beg);
    my_write(out_fd, broken_pt, broken_len - (broken_pt - broken_beg));
}

int main(int ac, char **av)
{
    ac--; av++;
    if (ac != 3)
	usage();
    
    head_fname = *av;
    head_fd = my_open(head_fname, O_RDONLY, 0);
    head_len = get_file_len(head_fd);
    ac--; av++;

    broken_fname = *av;
    broken_fd = my_open(broken_fname, O_RDONLY, 0);
    broken_len = get_file_len(broken_fd);
    ac--; av++;

    out_fname = *av;
    ASSERT(!file_exists(out_fname),
	   "%s: File exists, aborting\n", out_fname);
    ac--; av++;    

    head_beg = my_mmap(0, head_len, PROT_READ, MAP_PRIVATE, head_fd, 0);
    broken_beg = my_mmap(0, broken_len, PROT_READ, MAP_PRIVATE, broken_fd, 0);

    doit();
    
    close(out_fd);
    
    return 0;
}
