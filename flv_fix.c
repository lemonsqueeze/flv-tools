
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
    printf("  flv_fix  file.flv out.flv\n");
    printf("\n");
    printf("  Attempt to repair invalid file.flv (flv_debug shows errors).\n");
    printf("  Output written to out.flv\n");
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

int file_exists(const char *name)
{
    struct stat st;
    if (stat(name, &st) == -1 &&
	errno == ENOENT)
	return 0;
    return 1;
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


int read_number(const uchar *pt, int bytes)
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

int parse_tag(const uchar *pt,
	      uchar *type, int *body_len, int *timestamp, int *stream_id,
	      const uchar *beg, int file_len)
{
    const uchar const *pt_orig = pt;
#define OFFSET	(pt_orig - beg)
    int prev_len = 0;

    // check we're within file boundaries.
    if (pt - beg + 15 > file_len)
    {
	printf("File boundaries exceeded.\n");
	return 0;
    }
    
    *type = *pt++;
    if (! (*type == FLV_TYPE_AUDIO ||
	   *type == FLV_TYPE_VIDEO ||
	   *type == FLV_TYPE_META))
    {
	printf("Invalid tag type %#02x at offset %i\n", *type, OFFSET);
	return 0;
    }
    
    *body_len = read_number(pt, 3);
    pt += 3;

    // Timestamp in milliseconds
    *timestamp = read_number(pt, 3);
    pt += 3;    

    *stream_id = read_number(pt, 4);
    pt += 4;
    
    printf("%08i: Found TAG type %#04x, len %5i, time %7i, stream_id %i\n",
	   OFFSET, *type, *body_len, *timestamp, *stream_id);

    /* Check end tag len */
    pt = skip_tag(pt_orig, *body_len);
    
    pt -= 4;
    // check we're within file boundaries.
    if (pt - beg + 4 > file_len)
    {
	printf("File boundaries exceeded.\n");
	return 0;
    }
    prev_len = read_number(pt, 4);
    if (prev_len + 4 != *body_len + 15)
    {
	printf("*** Warning: Invalid tag, end of tag length mismatch (%i != %i)\n",
	       prev_len + 4, *body_len + 15);
	return 0;
    }
    return 1;
}

uchar		*head_beg = 0;
const char	*head_fname = 0;
int		head_fd = 0;
int		head_len = 0;

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


void parse_tags()
{
    const uchar const *beg = head_beg;
    const uchar *pt = beg;
    const uchar *next_pt = 0;
    uchar type;
    int len, timestamp, stream_id;
    int prev_timestamp = 0;

    /* Checking head */
    if (!strncmp((char*)pt, "FLV", 3))
    {
	my_write(out_fd, pt, 13);
	pt += 13;
    }
    else
	printf("file %s: invalid FLV header\n", head_fname);
    
    if (*pt != FLV_TYPE_META)
	printf("Warning: Non metadata tag (%#02x) at offset 13\n", *pt);    

    while (pt - beg < head_len)
    {
	if (!parse_tag(pt, &type, &len, &timestamp, &stream_id, beg, head_len))
	{ 
	    pt++; // invalid tag, try to find next one ...
	    continue;
	}

	if (timestamp < prev_timestamp)
	{
	    printf("Warning: backward timestamp, skipping.\n");
	    pt++;
	    continue;
	}
	prev_timestamp = timestamp;
	
	// TODO: parse metadata tag
	
//	printf("TAG type %#02x, len %i\n", type, len);
	ASSERT(*pt == FLV_TYPE_AUDIO ||
	       *pt == FLV_TYPE_VIDEO ||
	       *pt == FLV_TYPE_META,
	       "Unknown tag ... corrupted file ?\n");

	next_pt = skip_tag(pt, len);
	my_write(out_fd, pt, next_pt - pt);
	pt = next_pt;
    }
}

int main(int ac, char **av)
{
    ac--; av++;
    if (ac != 2)
	usage();
    
    head_fname = *av;
    head_fd = my_open(head_fname, O_RDONLY, 0);
    head_len = get_file_len(head_fd);
    ac--; av++;

    out_fname = *av;
    ASSERT(!file_exists(out_fname),
	   "%s: File exists, aborting\n", out_fname);
    out_fd = my_open(out_fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);        
    ac--; av++;    
    
    head_beg = my_mmap(0, head_len, PROT_READ, MAP_PRIVATE, head_fd, 0);

    parse_tags();

    close(out_fd);    
    
    return 0;
}
