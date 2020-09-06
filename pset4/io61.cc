#include "io61.hh"
#include <sys/types.h>
#include <sys/stat.h>
#include <climits>
#include <cerrno>

#define cach_size 16384 //4x4096 looked like a good size
// io61.c


// io61_file
//    Data structure for io61 file wrappers.

struct io61_file {
    int fd;
    //The cache buffer, tags, and the mode:
    char cach[cach_size];
    off_t start_tag;
    off_t end_tag;
    off_t cur_tag;
    int mode;
};


// io61_fdopen(fd, mode)
//    Return a new io61_file for file descriptor `fd`. `mode` is
//    either O_RDONLY for a read-only file or O_WRONLY for a
//    write-only file. You need not support read/write files.

io61_file* io61_fdopen(int fd, int mode) {
    assert(fd >= 0);
    io61_file* f = new io61_file;
    f->fd = fd;
    f->mode=mode; //We will need the more in io61_seek
    f->cur_tag=f->end_tag=f->start_tag=0;
    (void) mode;
    return f;
}


// io61_close(f)
//    Close the io61_file `f` and release all its resources.

int io61_close(io61_file* f) {
    io61_flush(f);
    int r = close(f->fd);
    delete f;
    return r;
}

// void io61_fill(io61_file* f)
void io61_fill(io61_file* f){
    //Move to the end:
    f->cur_tag=f->end_tag;
    //Move end_tag backwards so that it is cach_size alligned:
    f->end_tag=(f->end_tag/cach_size)*cach_size;
    //We will start reading from this alligned position:
    f->start_tag=f->end_tag;
    lseek(f->fd,f->end_tag,SEEK_SET);
    int sz=read(f->fd,f->cach,cach_size);
    if (sz>0)
        f->end_tag+=sz;
    else f->end_tag=f->start_tag=f->cur_tag;
}

// io61_readc(f)
//    Read a single (unsigned) character from `f` and return it. Returns EOF
//    (which is -1) on error or end-of-file.

int io61_readc(io61_file* f) {
    //Can we fullfill this read without making system calls:
    if (f->cur_tag<f->end_tag){
        unsigned char buf=f->cach[f->cur_tag-f->start_tag];
        f->cur_tag++;
        return buf;
    }
    //Fill the cache to fullfill the read:
    io61_fill(f);
    if (f->cur_tag<f->end_tag){
        unsigned char buf=f->cach[f->cur_tag-f->start_tag];
        f->cur_tag++;
        return buf;
    }
    //If nothing was filled then this is the EOF:
    return EOF;
}





// io61_read(f, buf, sz)
//    Read up to `sz` characters from `f` into `buf`. Returns the number of
//    characters read on success; normally this is `sz`. Returns a short
//    count, which might be zero, if the file ended before `sz` characters
//    could be read. Returns -1 if an error occurred before any characters
//    were read.

ssize_t io61_read(io61_file* f, char* buf, size_t sz) {
    //Can we fullfill this read from the cache?
    if ((signed)sz<=f->end_tag-f->cur_tag){
        memcpy((void*)buf,(void*)&f->cach[f->cur_tag-f->start_tag],sz);
        f->cur_tag+=sz;
        return sz;
    }
    //Let's read the bytes we already have:
    memcpy((void*)buf,(void*)&f->cach[f->cur_tag-f->start_tag],f->end_tag-f->cur_tag);
    size_t nread = f->end_tag-f->cur_tag;
    if (sz-nread>cach_size){
        //If one more fill will still not be enough then it is better
        //to read directly from the file instead of having a loop of fills
        nread+=read(f->fd,(void*)&buf[nread],sz-nread);
        f->cur_tag+=nread;
        f->end_tag=f->start_tag=f->cur_tag;
        return nread;
    }
    //Do a fill and then get the rest of the bytes that you want:
    f->cur_tag+=nread;
    io61_fill(f);
    if ((int)(sz-nread)<=f->end_tag-f->cur_tag){
        //Can we get all the bytes we need?:
        memcpy((void*)&buf[nread],(void*)&f->cach[f->cur_tag-f->start_tag],sz-nread);
        f->cur_tag+=sz-nread;
        return sz;
    }
    //Get the rest of the bytes available:
    memcpy((void*)&buf[nread],(void*)&f->cach[f->cur_tag-f->start_tag],f->end_tag-f->cur_tag);
    f->cur_tag=f->end_tag;
    return nread+f->end_tag-f->cur_tag;

}


// io61_writec(f)
//    Write a single character `ch` to `f`. Returns 0 on success or
//    -1 on error.

int io61_writec(io61_file* f, int ch) {
    //Can we still write to the cache?
    if (f->end_tag-f->start_tag<cach_size){
        f->cach[f->end_tag-f->start_tag]=ch;
        f->end_tag++;
        return 0;
    }
    //If our cache is full then we flush and write to the cach:
    io61_flush(f);
    f->cach[0]=ch;
    f->end_tag++;
    return 0;
}


// io61_write(f, buf, sz)
//    Write `sz` characters from `buf` to `f`. Returns the number of
//    characters written on success; normally this is `sz`. Returns -1 if
//    an error occurred before any characters were written.

ssize_t io61_write(io61_file* f, const char* buf, size_t sz) {
    size_t cach_used=f->end_tag-f->start_tag;
    //Can we write it all to the cache?
    if (cach_size-cach_used>=sz){
        memcpy((void*)&f->cach[cach_used],(void*)buf,sz);
        f->end_tag+=sz;
        return sz;
    }
    if (sz-(cach_size-cach_used)>=cach_size){
        //If the remaining bytes in the cache plus one more cache will
        //not be enough for all our writes then it's better to write
        //directly to the file:
        io61_flush(f);
        size_t nwritten = write(f->fd,(void*)buf,sz);
        if (nwritten != 0 || sz == 0) {
            f->end_tag+=nwritten;
            f->start_tag=f->end_tag;
            return nwritten;
        } else {
            return -1;
        }
    }
    //Let's fill the rest of this cache and flush it:
    memcpy((void*)&f->cach[cach_used],(void*)buf,cach_size-cach_used);
    f->end_tag=f->start_tag+cach_size;
    io61_flush(f);
    //Then fill a new cache with the rest of our writes:
    memcpy((void*)f->cach,(void*)&buf[cach_size-cach_used],sz-(cach_size-cach_used));
    f->end_tag+=sz-(cach_size-cach_used);
    return sz;
}


// io61_flush(f)
//    Forces a write of all buffered data written to `f`.
//    If `f` was opened read-only, io61_flush(f) may either drop all
//    data buffered for reading, or do nothing.

int io61_flush(io61_file* f) {
    (void) f;
    if (f->end_tag>f->start_tag){
        int nwritten=write(f->fd,(void*)f->cach,f->end_tag-f->start_tag);
        if (nwritten<=0){
            f->start_tag = f->cur_tag = f->end_tag;
            return -1;
        }
    }
    f->start_tag = f->cur_tag = f->end_tag;
    return 0;
}

// io61_seek(f, pos)
//    Change the file pointer for file `f` to `pos` bytes into the file.
//    Returns 0 on success and -1 on failure.

int io61_seek(io61_file* f, off_t pos) {
    if (f->mode==O_RDONLY){
        //Is the desired position within our cache:
        if (pos>=f->start_tag && pos<f->end_tag){
            f->cur_tag=pos;
            return 0;
        }
        //Let's seek to the new position and update the tags:
        off_t r = lseek(f->fd, (off_t) pos, SEEK_SET);
        if (r == (off_t) pos) {
            f->end_tag=f->start_tag=f->cur_tag=pos;
            return 0;
        }
        else {
            return -1;
        }

    }
    //If it is a write file we should flush, seek, and update the tags:
    io61_flush(f);
    off_t r = lseek(f->fd, (off_t) pos, SEEK_SET);
    if (r == (off_t) pos) {
        f->end_tag=f->start_tag=pos;
        return 0;
    }
    else {
        return -1;
    }
}


// You shouldn't need to change these functions.

// io61_open_check(filename, mode)
//    Open the file corresponding to `filename` and return its io61_file.
//    If `!filename`, returns either the standard input or the
//    standard output, depending on `mode`. Exits with an error message if
//    `filename != nullptr` and the named file cannot be opened.

io61_file* io61_open_check(const char* filename, int mode) {
    int fd;
    if (filename) {
        fd = open(filename, mode, 0666);
    } else if ((mode & O_ACCMODE) == O_RDONLY) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", filename, strerror(errno));
        exit(1);
    }
    return io61_fdopen(fd, mode & O_ACCMODE);
}


// io61_filesize(f)
//    Return the size of `f` in bytes. Returns -1 if `f` does not have a
//    well-defined size (for instance, if it is a pipe).

off_t io61_filesize(io61_file* f) {
    struct stat s;
    int r = fstat(f->fd, &s);
    if (r >= 0 && S_ISREG(s.st_mode)) {
        return s.st_size;
    } else {
        return -1;
    }
}
