
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>


#define uchar unsigned char

#define PARSE_BROKEN_FILE 1
//#undef PARSE_BROKEN_FILE

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
    printf("  flv_debug  file.flv\n");
    printf("\n");
    exit(1);
}

#define ASSERT(check, format, args...)  do  {	\
	if (!(check))				\
	{ printf(format, ##args); /*exit(1);*/ }	\
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

char time_buf[20];
char time_buf2[20];

char* format_time(int time, char *str)
{
    int m, s, ms;
    ms = time % 1000;
    s = (time / 1000) % 60;
    m = time / (60 * 1000);
    sprintf(str, "%02i:%02i:%03i", m, s, ms);
    return str;
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
    // FIXME: should check we're within the file boundaries
#define OFFSET	(pt_orig - beg)
    int prev_len = 0;
    
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
    
//    printf("%08i: Found TAG type %#04x, len %5i, time %s, stream_id %i\n",
//	   OFFSET, *type, *body_len, format_time(timestamp, time_buf), *stream_id);

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

int min_times[20] = {0,};
int max_times[20] = {0,};

void parse_tags()
{
    const uchar const *beg = head_beg;
    const uchar *pt = beg;
    uchar type;
    int len, timestamp, stream_id;
    int prev_time = -1;
    int i, time_range_idx = 0;
#define MIN_TIME (min_times[time_range_idx])
#define MAX_TIME (max_times[time_range_idx])
    
    /* Checking head */
    ASSERT(!strncmp((char*)pt, "FLV", 3), "file %s: invalid FLV header\n", head_fname);
    pt += 13;
    
    if (*pt != FLV_TYPE_META)
	printf("Warning: Non metadata tag (%#02x) at offset 13\n", *pt);    

    while (pt - beg < head_len)
    {
	if (!parse_tag(pt, &type, &len, &timestamp, &stream_id, beg, head_len))
	{
#ifdef PARSE_BROKEN_FILE
	    pt++;
	    continue;
#else
	    printf("Broken file, stopping here (at %i%%).\n",
		   (pt - beg) * 100 / head_len);
	    break;
#endif
	}
	
	if (prev_time != -1 &&
	    timestamp - prev_time > 500)
	{
	    printf("WARNING: Time gap in file (jump by %s)\n",
		   format_time(timestamp - prev_time, time_buf));
	    time_range_idx++;
	    MIN_TIME = timestamp;
	}
	prev_time = timestamp;

	if (timestamp < MIN_TIME)
	    MIN_TIME = timestamp;
	if (timestamp > MAX_TIME)
	    MAX_TIME = timestamp;
	
	printf("%08i: Found TAG type %#04x, len %5i, time %s, stream_id %i\n",
	       pt - beg, type, len, format_time(timestamp, time_buf), stream_id);

	// TODO: parse metadata tag
	
//	printf("TAG type %#02x, len %i\n", type, len);
	ASSERT(*pt == FLV_TYPE_AUDIO ||
	       *pt == FLV_TYPE_VIDEO ||
	       *pt == FLV_TYPE_META,
	       "Unknown tag ... corrupted file ?\n");
	pt = skip_tag(pt, len);
    }

    printf("Time range: ");
    for (i = 0; i <= time_range_idx; i++)
	printf("[%s, %s] ",
	       format_time(min_times[i], time_buf),
	       format_time(max_times[i], time_buf2));
    printf("\n");
    if (time_range_idx)
	printf("WARNING: %i time gap(s) found.\n", time_range_idx);
    
}

int main(int ac, char **av)
{
    ac--; av++;
    if (ac != 1)
	usage();
    
    head_fname = *av;
    head_fd = my_open(head_fname, O_RDONLY, 0);
    head_len = get_file_len(head_fd);
    ac--; av++;

    head_beg = my_mmap(0, head_len, PROT_READ, MAP_PRIVATE, head_fd, 0);

    parse_tags();
    
    return 0;
}
