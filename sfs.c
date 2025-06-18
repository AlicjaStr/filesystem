#define FUSE_USE_VERSION 26

#include <errno.h>
#include <fuse.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "sfs.h"
#include "diskio.h"


static const char default_img[] = "test.img";

struct options {
    const char *img;
    int background;
    int verbose;
    int show_help;
    int show_fuse_help;
} options;


#define log(fmt, ...) \
    do { \
        if (options.verbose) \
            printf(" # " fmt, ##__VA_ARGS__); \
    } while (0)


const char* __asan_default_options() { return "detect_leaks=0"; }


static int get_entry_rec(const char *path, const struct sfs_entry *parent,
                         size_t parent_nentries, blockidx_t parent_blockidx,
                         struct sfs_entry *ret_entry, unsigned *ret_entry_off) {

    if(!path || !parent) 
        return -EINVAL;  

    char *path_copy = strdup(path);  
    char *current = strtok(path_copy, "/");  
    char *next = strtok(NULL, ""); 

    struct sfs_entry root_dir[SFS_ROOTDIR_NENTRIES];
    struct sfs_entry sub_dir[SFS_DIR_NENTRIES];

    if(parent_nentries == SFS_ROOTDIR_NENTRIES) {
        disk_read(root_dir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);

        for(size_t i = 0; i < SFS_ROOTDIR_NENTRIES; i++) {
            if(strcmp(current, root_dir[i].filename) == 0) {
                if(!next) {  
                    *ret_entry = root_dir[i];
                    *ret_entry_off = SFS_ROOTDIR_OFF + i * sizeof(struct sfs_entry);
                    return 0;
                } 
                else 
                    return get_entry_rec(next, root_dir, SFS_DIR_NENTRIES, root_dir[i].first_block, ret_entry, ret_entry_off);
            }
        }
        return -1;
    } 
    else if(parent_nentries == SFS_DIR_NENTRIES) {
        disk_read(sub_dir, SFS_DIR_SIZE, SFS_DATA_OFF + parent_blockidx * SFS_BLOCK_SIZE);

        for(size_t i = 0; i < SFS_DIR_NENTRIES; i++) {
            if(strcmp(current, sub_dir[i].filename) == 0) {
                if(!next) {  
                    *ret_entry = sub_dir[i];
                    *ret_entry_off = SFS_DATA_OFF + parent_blockidx * SFS_BLOCK_SIZE + i * sizeof(struct sfs_entry);
                    return 0;
                } 
                else 
                    return get_entry_rec(next, sub_dir, SFS_DIR_NENTRIES, sub_dir[i].first_block, ret_entry, ret_entry_off);
            }
        }
        return -1;
    }
    return -ENOENT;
}


static int get_entry(const char *path, struct sfs_entry *ret_entry,
                     unsigned *ret_entry_off)
{
    struct sfs_entry root_dir[SFS_ROOTDIR_NENTRIES];
    disk_read(root_dir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);
    return get_entry_rec(path, root_dir, SFS_ROOTDIR_NENTRIES, 0, ret_entry, ret_entry_off);
}


static int sfs_getattr(const char *path,
                       struct stat *st)
{
    log("getattr %s\n", path);

    int res = 0;  
    struct sfs_entry entry;
    unsigned entry_off; 

    memset(st, 0, sizeof(struct stat));

    st->st_uid = getuid();  
    st->st_gid = getgid();  

    st->st_atime = time(NULL);  
    st->st_mtime = time(NULL);  

    if(strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;  
        st->st_nlink = 2;  
    } 
    else { 
        res = get_entry(path, &entry, &entry_off);

        if(res == 0) {
            if(entry.size & SFS_DIRECTORY) {
                st->st_mode = S_IFDIR | 0755; 
                st->st_nlink = 2;  
            } 
            else {
                st->st_mode = S_IFREG | 0644; 
                st->st_nlink = 1;  
                st->st_size = entry.size & SFS_SIZEMASK;  
            }
        } 
        else 
            res = -ENOENT;  
    }
    return res;
}

static int sfs_readdir(const char *path,
                       void *buf,
                       fuse_fill_dir_t filler,
                       off_t offset,
                       struct fuse_file_info *fi)
{
    (void)offset, (void)fi;
    log("readdir %s\n", path);

    struct sfs_entry root_dir[SFS_ROOTDIR_NENTRIES];
    struct sfs_entry sub_dir[SFS_DIR_NENTRIES];

    if(strcmp(path, "/") == 0) {
        filler(buf, ".", NULL, 0);
        filler(buf, "..", NULL, 0);

        disk_read(root_dir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);

        for(size_t i = 0; i < SFS_ROOTDIR_NENTRIES; i++) {
            if(root_dir[i].filename[0] == '\0') 
                continue; 
            filler(buf, root_dir[i].filename, NULL, 0);
        }
        return 0;
    } 
    else {
        struct sfs_entry entry;
        unsigned entry_off;

        if(get_entry(path, &entry, &entry_off) == 0) {
            if(entry.size & SFS_DIRECTORY) {
                disk_read(sub_dir, SFS_DIR_SIZE, SFS_DATA_OFF + entry.first_block * SFS_BLOCK_SIZE);

                filler(buf, ".", NULL, 0);
                filler(buf, "..", NULL, 0);

                for(size_t i = 0; i < SFS_DIR_NENTRIES; i++) {
                    if (sub_dir[i].filename[0] == '\0') continue;  
                    filler(buf, sub_dir[i].filename, NULL, 0);
                }
                return 0;
            } 
        } 
    }
    return -ENOENT;  
}

static int sfs_read(const char *path,
                    char *buf,
                    size_t size,
                    off_t offset,
                    struct fuse_file_info *fi)
{
    (void)fi;
    log("read %s size=%zu offset=%ld\n", path, size, offset);

    struct sfs_entry entry;
    unsigned entry_offset;

    if(get_entry(path, &entry, &entry_offset) != 0 || (entry.size & SFS_DIRECTORY)) 
        return -EISDIR; 

    size_t file_size = entry.size & SFS_SIZEMASK;

    if((size_t)offset >= file_size) 
        return 0; 

    if(size + (size_t)offset > file_size) 
        size = file_size - offset;

    blockidx_t block_table[SFS_BLOCKTBL_NENTRIES];
    disk_read(block_table, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);
    size_t buffer_offset = 0;
    char block_buffer[SFS_BLOCK_SIZE];

    blockidx_t block = entry.first_block;
    size_t block_offset = offset % SFS_BLOCK_SIZE; 
    off_t block_index = offset / SFS_BLOCK_SIZE; 

    while(block_index > 0 && block != SFS_BLOCKIDX_END) {
        block = block_table[block];
        block_index--; 

        if(block == SFS_BLOCKIDX_END) 
            break; 
    }

    while(size > 0 && block != SFS_BLOCKIDX_END) {
        disk_read(block_buffer, SFS_BLOCK_SIZE, SFS_DATA_OFF + block * SFS_BLOCK_SIZE);
        size_t bytes_block = SFS_BLOCK_SIZE - block_offset;

        if(bytes_block > size) 
            bytes_block = size;

        memcpy(buf + buffer_offset, block_buffer + block_offset, bytes_block);

        size -= bytes_block;
        buffer_offset += bytes_block;
        block_offset = 0; 
        block = block_table[block];
    }
    return buffer_offset; 
}


static int sfs_mkdir(const char *path,
                     mode_t mode)
{
    log("mkdir %s mode=%o\n", path, mode);

    char *path_copy = strdup(path);
    char *path_end = strrchr(path_copy, '/'); 
    *path_end = '\0';
    char *parent_path = path_copy; 
    char *new_dir = path_end + 1; 

    if(strlen(parent_path) == 0) 
        parent_path = "/";

    if(strlen(new_dir) >= SFS_FILENAME_MAX) 
        return -ENAMETOOLONG; 

    struct sfs_entry root_dir[SFS_ROOTDIR_NENTRIES]; 
    disk_read(root_dir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF); 
    
    blockidx_t block_table[SFS_BLOCKTBL_NENTRIES];
    disk_read(block_table, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);

    blockidx_t first_block = SFS_BLOCKIDX_EMPTY; 
    blockidx_t second_block = SFS_BLOCKIDX_EMPTY;

    for(unsigned int i = 0; i < SFS_BLOCKTBL_NENTRIES - 1; i++) { 
        if(block_table[i] == SFS_BLOCKIDX_EMPTY && block_table[i + 1] == SFS_BLOCKIDX_EMPTY) {
            first_block = i;
            second_block = i + 1;
            block_table[first_block] = second_block; 
            block_table[second_block] = SFS_BLOCKIDX_END; 
            disk_write(block_table, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);
            break;
        }
    }

    if(strcmp(parent_path, "/") == 0) { 
        for(unsigned i = 0; i < SFS_ROOTDIR_NENTRIES; ++i) { 
            if(strlen(root_dir[i].filename) == 0) { 
                root_dir[i].first_block = first_block;
                root_dir[i].size = SFS_DIRECTORY;
                strcpy(root_dir[i].filename, new_dir); 
                disk_write(root_dir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);

                struct sfs_entry new_entries[SFS_DIR_NENTRIES];

                for(unsigned i = 0; i < SFS_DIR_NENTRIES; i++) {
                    new_entries[i].size = 0;
                    new_entries[i].first_block = SFS_BLOCKIDX_EMPTY;
                    strcpy(new_entries[i].filename, "\0");
                }
                disk_write(new_entries, SFS_DIR_SIZE, SFS_DATA_OFF + first_block * SFS_BLOCK_SIZE);
                return 0;
            }
        }
    }
    else { 
        struct sfs_entry new_entry;
        unsigned new_entry_offset;
        int parent_entry = get_entry(parent_path, &new_entry, &new_entry_offset); 

        if(parent_entry == 0) {
            struct sfs_entry entries[SFS_DIR_NENTRIES];
            disk_read(entries, SFS_DIR_SIZE, SFS_DATA_OFF + new_entry.first_block * SFS_BLOCK_SIZE);

            for(unsigned i = 0; i < SFS_DIR_NENTRIES; ++i) {
                if(strlen(entries[i].filename) == 0) {
                    entries[i].first_block = first_block;
                    entries[i].size = SFS_DIRECTORY;
                    strcpy(entries[i].filename, new_dir);
                    disk_write(entries, SFS_DIR_SIZE, SFS_DATA_OFF + new_entry.first_block * SFS_BLOCK_SIZE);

                    struct sfs_entry new_entries[SFS_DIR_NENTRIES];
                    
                    for(unsigned i = 0; i < SFS_DIR_NENTRIES; i++) {
                        new_entries[i].size = 0;
                        new_entries[i].first_block = SFS_BLOCKIDX_EMPTY;
                        strcpy(new_entries[i].filename, "\0");
                    }
                    disk_write(new_entries, SFS_DIR_SIZE, SFS_DATA_OFF + first_block * SFS_BLOCK_SIZE);
                    return 0;
                }
            }
        }
    }
    return -1;
}

static int sfs_rmdir(const char *path)
{
    log("rmdir %s\n", path);
    
    char *path_copy = strdup(path);
    char *path_end = strrchr(path_copy, '/');
    *path_end = '\0';
    char *parent_path = path_copy;
    char *dir = path_end + 1;
    
    if(strlen(parent_path) == 0) 
        parent_path = "/";

    struct sfs_entry root_dir[SFS_ROOTDIR_NENTRIES];
    disk_read(root_dir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);

    blockidx_t block_table[SFS_BLOCKTBL_NENTRIES];
    disk_read(block_table, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);

    struct sfs_entry dir_entry;
    unsigned dir_entry_offset;
    int res = get_entry(path, &dir_entry, &dir_entry_offset);

    struct sfs_entry dir_entries[SFS_DIR_NENTRIES];
    disk_read(dir_entries, SFS_DIR_SIZE, SFS_DATA_OFF + dir_entry.first_block * SFS_BLOCK_SIZE);

    for(unsigned i = 0; i < SFS_DIR_NENTRIES; ++i) {
        if(dir_entries[i].filename[0] != '\0') 
            return -ENOTEMPTY; 
    }

    if(strcmp(parent_path, "/") == 0) {
        for(unsigned i = 0; i < SFS_ROOTDIR_NENTRIES; ++i) {
            if(strcmp(root_dir[i].filename, dir) == 0) {
                root_dir[i].first_block = SFS_BLOCKIDX_EMPTY;
                root_dir[i].size = 0;
                strcpy(root_dir[i].filename, "\0"); 
                disk_write(root_dir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);
                break;
            }
        }
    } 
    else {
        struct sfs_entry parent_entry;
        unsigned parent_entry_offset;
        res = get_entry(parent_path, &parent_entry, &parent_entry_offset);

        if(res != 0) 
            return -ENOENT; 
        
        struct sfs_entry parent_entries[SFS_DIR_NENTRIES];
        disk_read(parent_entries, SFS_DIR_SIZE, SFS_DATA_OFF + parent_entry.first_block * SFS_BLOCK_SIZE);

        for(unsigned i = 0; i < SFS_DIR_NENTRIES; ++i) {
            if(strcmp(parent_entries[i].filename, dir) == 0) {
                parent_entries[i].first_block = SFS_BLOCKIDX_EMPTY;
                parent_entries[i].size = 0;
                strcpy(parent_entries[i].filename, "\0"); 
                disk_write(parent_entries, SFS_DIR_SIZE, SFS_DATA_OFF + parent_entry.first_block * SFS_BLOCK_SIZE);
                break;
            }
        }
    }

    blockidx_t block = dir_entry.first_block;
    while(block != SFS_BLOCKIDX_END) {
        blockidx_t next = block_table[block];
        block_table[block] = SFS_BLOCKIDX_EMPTY;
        block = next;
    }

    disk_write(block_table, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);
    return 0; 
}

static int sfs_unlink(const char *path)
{
    log("unlink %s\n", path);

    char *path_copy = strdup(path);
    char *path_end = strrchr(path_copy, '/');
    *path_end = '\0';
    char *parent_path = path_copy;
    char *file = path_end + 1;

    if(strlen(parent_path) == 0)
        parent_path = "/";

    struct sfs_entry root_dir[SFS_ROOTDIR_NENTRIES];
    disk_read(root_dir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);

    blockidx_t block_table[SFS_BLOCKTBL_NENTRIES];
    disk_read(block_table, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);

    struct sfs_entry file_entry;
    unsigned file_entry_offset;
    int res = get_entry(path, &file_entry, &file_entry_offset);
   
    if(strcmp(parent_path, "/") == 0) {
        for(unsigned i = 0; i < SFS_ROOTDIR_NENTRIES; ++i) {
            if (strcmp(root_dir[i].filename, file) == 0) {
                root_dir[i].filename[0] = '\0'; 
                root_dir[i].first_block = SFS_BLOCKIDX_EMPTY;
                root_dir[i].size = 0;
                disk_write(root_dir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);
                break;
            }
        }
    } 
    else {
        struct sfs_entry parent_entry;
        unsigned parent_entry_offset;
        res = get_entry(parent_path, &parent_entry, &parent_entry_offset);

        if(res != 0) 
            return -ENOENT; 

        struct sfs_entry parent_entries[SFS_DIR_NENTRIES];
        disk_read(parent_entries, SFS_DIR_SIZE, SFS_DATA_OFF + parent_entry.first_block * SFS_BLOCK_SIZE);

        for (unsigned i = 0; i < SFS_DIR_NENTRIES; ++i) {
            if (strcmp(parent_entries[i].filename, file) == 0) {
                parent_entries[i].filename[0] = '\0';
                parent_entries[i].first_block = SFS_BLOCKIDX_EMPTY;
                parent_entries[i].size = 0;
                disk_write(parent_entries, SFS_DIR_SIZE, SFS_DATA_OFF + parent_entry.first_block * SFS_BLOCK_SIZE);
                break;
            }
        }
    }

    blockidx_t block = file_entry.first_block;
    while(block != SFS_BLOCKIDX_END) {
        blockidx_t next = block_table[block];
        block_table[block] = SFS_BLOCKIDX_EMPTY;
        block = next;
    }

    disk_write(block_table, SFS_BLOCKTBL_SIZE, SFS_BLOCKTBL_OFF);
    return 0; 
}


static int sfs_create(const char *path,
                      mode_t mode,
                      struct fuse_file_info *fi)
{
    (void)fi; 
    log("create %s mode=%o\n", path, mode);

    char *path_copy = strdup(path);
    char *path_end = strrchr(path_copy, '/');
    *path_end = '\0';
    char *parent_path = path_copy;
    char *file = path_end + 1;

    if(strlen(file) >= SFS_FILENAME_MAX) 
        return -ENAMETOOLONG;

    if (strlen(parent_path) == 0)
        parent_path = "/";

    struct sfs_entry root_dir[SFS_ROOTDIR_NENTRIES];
    disk_read(root_dir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);

    if(strcmp(parent_path, "/") == 0) {
        for(unsigned i = 0; i < SFS_ROOTDIR_NENTRIES; ++i) {
            if(strlen(root_dir[i].filename) == 0) {
                root_dir[i].first_block = SFS_BLOCKIDX_END;
                root_dir[i].size = 0; 
                strcpy(root_dir[i].filename, file);
                disk_write(root_dir, SFS_ROOTDIR_SIZE, SFS_ROOTDIR_OFF);
                return 0; 
            }
        }
    } 
    else {
        struct sfs_entry parent_entry;
        unsigned parent_entry_offset;
        int res = get_entry(parent_path, &parent_entry, &parent_entry_offset);

        if(res != 0) 
            return -ENOENT; 

        struct sfs_entry parent_entries[SFS_DIR_NENTRIES];
        disk_read(parent_entries, SFS_DIR_SIZE, SFS_DATA_OFF + parent_entry.first_block * SFS_BLOCK_SIZE);

        for (unsigned i = 0; i < SFS_DIR_NENTRIES; ++i) {
            if (strlen(parent_entries[i].filename) == 0) { 
                parent_entries[i].first_block = SFS_BLOCKIDX_END; 
                parent_entries[i].size = 0; 
                strcpy(parent_entries[i].filename, file);
                disk_write(parent_entries, SFS_DIR_SIZE, SFS_DATA_OFF + parent_entry.first_block * SFS_BLOCK_SIZE);
                return 0;
            }
        }
    }
    return -ENOSPC; 
}


/*
 * Shrink or grow the file at `path` to `size` bytes.
 * Excess bytes are thrown away, whereas any bytes added in the process should
 * be nil (\0).
 * Returns 0 on success, < 0 on error.
 */
static int sfs_truncate(const char *path, off_t size)
{
    log("truncate %s size=%ld\n", path, size);

    return -ENOSYS;
}


/*
 * Write contents of `buf` (of `size` bytes) to the file at `path`.
 * The file is grown if nessecary, and any bytes already present are overwritten
 * (whereas any other data is left intact). The `offset` argument specifies how
 * many bytes should be skipped in the file, after which `size` bytes from
 * buffer are written.
 * This means that the new file size will be max(old_size, offset + size).
 * Returns the number of bytes written, or < 0 on error.
 */
static int sfs_write(const char *path,
                     const char *buf,
                     size_t size,
                     off_t offset,
                     struct fuse_file_info *fi)
{
    (void)fi;
    log("write %s data='%.*s' size=%zu offset=%ld\n", path, (int)size, buf,
        size, offset);

    return -ENOSYS;
}


/*
 * Move/rename the file at `path` to `newpath`.
 * Returns 0 on succes, < 0 on error.
 */
static int sfs_rename(const char *path,
                      const char *newpath)
{
    /* Implementing this function is optional, and not worth any points. */
    log("rename %s %s\n", path, newpath);

    return -ENOSYS;
}


static const struct fuse_operations sfs_oper = {
    .getattr    = sfs_getattr,
    .readdir    = sfs_readdir,
    .read       = sfs_read,
    .mkdir      = sfs_mkdir,
    .rmdir      = sfs_rmdir,
    .unlink     = sfs_unlink,
    .create     = sfs_create,
    .truncate   = sfs_truncate,
    .write      = sfs_write,
    .rename     = sfs_rename,
};


#define OPTION(t, p)                            \
    { t, offsetof(struct options, p), 1 }
#define LOPTION(s, l, p)                        \
    OPTION(s, p),                               \
    OPTION(l, p)
static const struct fuse_opt option_spec[] = {
    LOPTION("-i %s",    "--img=%s",     img),
    LOPTION("-b",       "--background", background),
    LOPTION("-v",       "--verbose",    verbose),
    LOPTION("-h",       "--help",       show_help),
    OPTION(             "--fuse-help",  show_fuse_help),
    FUSE_OPT_END
};

static void show_help(const char *progname)
{
    printf("usage: %s mountpoint [options]\n\n", progname);
    printf("By default this FUSE runs in the foreground, and will unmount on\n"
           "exit. If something goes wrong and FUSE does not exit cleanly, use\n"
           "the following command to unmount your mountpoint:\n"
           "  $ fusermount -u <mountpoint>\n\n");
    printf("common options (use --fuse-help for all options):\n"
           "    -i, --img=FILE      filename of SFS image to mount\n"
           "                        (default: \"%s\")\n"
           "    -b, --background    run fuse in background\n"
           "    -v, --verbose       print debug information\n"
           "    -h, --help          show this summarized help\n"
           "        --fuse-help     show full FUSE help\n"
           "\n", default_img);
}

int main(int argc, char **argv)
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    options.img = strdup(default_img);

    fuse_opt_parse(&args, &options, option_spec, NULL);

    if (options.show_help) {
        show_help(argv[0]);
        return 0;
    }

    if (options.show_fuse_help) {
        assert(fuse_opt_add_arg(&args, "--help") == 0);
        args.argv[0][0] = '\0';
    }

    if (!options.background)
        assert(fuse_opt_add_arg(&args, "-f") == 0);

    disk_open_image(options.img);

    return fuse_main(args.argc, args.argv, &sfs_oper, NULL);
}
