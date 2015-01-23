
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

// Number of video frames to skip at beginning of tail.
//   we need this because seek may not happen immediately,
//   in which case tail will begin with a few frames
//   from the beginning of the file.
int		skip_frames = 100;

// same idea as skip_frames, but with timestamp.
int		time_clue = -1;


#define uchar unsigned char

#define FLV_TYPE_AUDIO 0x08
#define FLV_TYPE_VIDEO 0x09
#define FLV_TYPE_META 0x12

void die(char *str)
{
    printf(str);
    exit(1);
}

// FIXME
void usage(void)
{
    printf("Usage:\n");
    printf("  flv_merge [-s skip_frames] [-t mm:ss:ms] head.flv   tail.flv   out.flv\n");
    printf("\n");
    printf("  Merge overlapping head.flv and tail.flv into one file.\n");
    printf("\n");    
    printf("  This is useful when downloading a big video file in several parts:\n");
    printf("  As long as seeking is done carefully to ensure there is some overlap\n");
    printf("  between the parts, flv_merge can be used to put it all back together.\n");
    printf("\n");
    printf("  flv_merge first skips skip_frames video frames at the beginning	   \n");
    printf("  of tail.flv, takes the next video frame, searches head.flv for it,   \n");
    printf("  and merges the two files together.				   \n");
    printf("                                                                       \n");
    printf("  skip_frames is there to give the user some time to do the seeking	   \n");
    printf("  in case the content prior to seeking shows up in the parts.          \n");
    printf("  Using a bad value for skip_frames is safe as there is a warning\n");
    printf("  if cutting point is found at the beginning of the file.\n");
    printf("\n");
    printf("  Instead of -s, -t can be used to give a time clue (much easier to use)\n");
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

int parse_tag(const uchar *pt, uchar *type, int *body_len, int *timestamp,
	      const uchar *beg)
{
    const uchar const *pt_orig = pt;
    // FIXME: should check we're within the file boundaries
#define OFFSET	(pt_orig - beg)
    int prev_len = 0;
    *type = *pt++;
    ASSERT(*type == FLV_TYPE_AUDIO ||
	   *type == FLV_TYPE_VIDEO ||
	   *type == FLV_TYPE_META,
	   "Invalid tag type %#02x at offset %i", *type, OFFSET);

    *body_len = read_number(pt, 3);
    pt += 3;

    // Timestamp in milliseconds
    *timestamp = read_number(pt, 3);
    pt += 3;        

//    printf("%#08x: Found TAG type %#02x, len %i\n", OFFSET, *type, *body_len);

    /* Check end tag len */
    pt = skip_tag(pt_orig, *body_len);
    
    pt -= 4;
    prev_len = read_number(pt, 4);
    if (prev_len + 4 != *body_len + 15)
    {
	printf("*** Warning: Invalid tag, end of tag length mismatch (%i != %i)\n",
	       prev_len + 4, *body_len + 15);
	return -1;
    }
    return 0;
}

uchar		*head_beg = 0;
const char	*head_fname = 0;
int		head_fd = 0;
int		head_len = 0;

uchar		*tail_beg = 0;
const char	*tail_fname = 0;
int		tail_fd = 0;
int		tail_len = 0;

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

const uchar* search_head(const uchar *search_pt, const int search_len)
{
    const uchar const *beg = head_beg;
    const uchar *pt = beg;
    uchar type;
    int len, timestamp;
    int time_min = 99999999, time_max = 0;
    const uchar *found = 0;    

    /* Checking head */
    ASSERT(!strncmp((char*)pt, "FLV", 3), "file %s: invalid FLV header\n", head_fname);
    pt += 13;

#if 0    
    ASSERT(*pt == FLV_TYPE_META, "Non metadata tag (%#02x) at offset 13\n", *pt);    
    parse_tag(pt, &type, &len, &timestamp, beg);
    pt = skip_tag(pt, len);

    ASSERT(*pt == FLV_TYPE_AUDIO ||
	   *pt == FLV_TYPE_VIDEO,
	   "hum, second tag neither audio or video ...\n");
#endif

    while (pt - beg < head_len)    
    {
	if (parse_tag(pt, &type, &len, &timestamp, beg) < 0)
	{
	    int percent = (pt - beg) * 100 / head_len;
	    printf("at %i%% of file, %s.\n", percent,
		   (percent > 95) ? "that's ok" : "stopping search there");
	    break; 
	}
	
	if (timestamp < time_min)
	    time_min = timestamp;
	if (timestamp > time_max)
	    time_max = timestamp;
	
	ASSERT(*pt == FLV_TYPE_AUDIO ||
	       *pt == FLV_TYPE_VIDEO ||
	       *pt == FLV_TYPE_META,
	       "Neither audio nor video tag ... corrupted file ?\n");	
//	printf("TAG type %#02x, len %i\n", type, len);

	// TODO: could check that there are not multiple matches ...	
	if (type == FLV_TYPE_VIDEO &&
//	    len == search_len &&
	    !memcmp(pt, search_pt, search_len))
	{
	    ASSERT(!found,
		   "Found multiple matches! Change skip_frames to use another frame!\n");
	    printf("Match found ! Making sure that's the only one ...\n");
	    found = pt;
	}
	
	pt = skip_tag(pt, len);	
    }
    printf("Time range scanned: [%s, %s]\n",
	   format_time(time_min, time_buf),
	   format_time(time_max, time_buf2));
    return found; // Not found
}

const uchar *next_tag(const uchar *pt, const uchar *beg)
{
    uchar type;
    int len, timestamp;

    ASSERT(parse_tag(pt, &type, &len, &timestamp, beg) == 0, "Aborting");
    ASSERT(type == FLV_TYPE_AUDIO ||
	   type == FLV_TYPE_VIDEO ||
	   type == FLV_TYPE_META,
	   "hum, tag neither audio or video ... corrupted file ?\n");    
    pt = skip_tag(pt, len);
    return pt;
}
	

// just skips audio frames if current one is not video
const uchar *get_next_video_frame(const uchar *pt, const uchar *beg)
{
    while (*pt != FLV_TYPE_VIDEO)
	pt = next_tag(pt, beg);
    return pt;	
}

// Get in tail a video frame to search for in head
const uchar* get_search_video_frame(int *search_len, int *search_time)
{
    const uchar const *beg = tail_beg;
    const uchar *pt = beg;
    uchar type;    
    int i = 0;
    
    ASSERT(!strncmp((char*)pt, "FLV", 3), "file %s: invalid FLV header\n", tail_fname);
    pt += 13;

    pt = get_next_video_frame(pt, beg);
    for (i = 0; i < skip_frames; i++)
    {
	// FIXME, just make this prog smart and do the right thing without any time clue.
	if (time_clue != -1)
	{
	    parse_tag(pt, &type, search_len, search_time, beg);
	    if (abs(*search_time - time_clue) < 500)
		break;
	}
	pt = next_tag(pt, beg);
	pt = get_next_video_frame(pt, beg);
    }

    parse_tag(pt, &type, search_len, search_time, beg);
    printf("Will search for video frame at %s  (offset %i, len=%i)\n",
	   format_time(*search_time, time_buf), pt - beg, *search_len);

    return pt;
}

void doit()
{
    const uchar *head_pt;
    const uchar *search_pt;
    int search_len = 0;
    int search_time = 0;
    int out_len = 0;
    int head_percent = 0;

    if (time_clue == -1)
	printf("%s: skipping first %i video frames\n", tail_fname, skip_frames);
        // The video frame we'll be searching for in head:    
    search_pt = get_search_video_frame(&search_len, &search_time);
    printf("\n");

    printf("%s: Searching for matching video frame ...\n", head_fname);
    head_pt = search_head(search_pt, search_len);
    ASSERT(head_pt,
	   "Couldn't find common part. Make sure the two files are overlapping.\n"
	   "If they are, then try changing skip_frames\n");
    head_percent = (float)(head_pt - head_beg) * 100 / head_len;    
    printf("\nJunction point found at %i%% of file (offset %i)!\n",
	   head_percent, head_pt - head_beg);	
    printf("\n");

    out_len = (head_pt - head_beg) + (tail_len - (search_pt - tail_beg));
    ASSERT(out_len > head_len,
	   "Bad search frame, this will clobber head -> Increase skip_frames\n");

    if (head_percent < 80)
    {
	printf("*** Warning: Match usually is near the end (80%%-90%%)\n");
	printf("             Make sure resulting file is ok, otherwise increase skip_frames\n");
    }

    out_fd = my_open(out_fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);    
    printf("Writing %s\n", out_fname);
    my_write(out_fd, head_beg, head_pt - head_beg);
    my_write(out_fd, search_pt, tail_len - (search_pt - tail_beg));
}

int parse_time(const char *str)
{
    int m, s, ms;
    if (sscanf(str, "%i:%i:%i", &m, &s, &ms) != 3)
	die("bad time format");
    return m * 60000 + s * 1000 + ms;
}

int main(int ac, char **av)
{
    ac--; av++;

    if (ac >= 2 && !strcmp(*av, "-s"))
    {
	skip_frames = atoi(av[1]);	
	ac -= 2;
	av += 2;
    }

    if (ac >= 2 && !strcmp(*av, "-t"))
    {
	time_clue = parse_time(av[1]);
	skip_frames = 99999999;
	ac -= 2;
	av += 2;
    }

    if (ac != 3)
	usage();

    head_fname = *av;
    head_fd = my_open(head_fname, O_RDONLY, 0);
    head_len = get_file_len(head_fd);
    ac--; av++;

    tail_fname = *av;
    tail_fd = my_open(tail_fname, O_RDONLY, 0);
    tail_len = get_file_len(tail_fd);
    ac--; av++;


    out_fname = *av;
    ASSERT(!file_exists(out_fname),
	   "%s: File exists, aborting\n", out_fname);
    ac--; av++;    

    head_beg = my_mmap(0, head_len, PROT_READ, MAP_PRIVATE, head_fd, 0);
    tail_beg = my_mmap(0, tail_len, PROT_READ, MAP_PRIVATE, tail_fd, 0);

    doit();
    
    close(out_fd);
    
    return 0;
}
