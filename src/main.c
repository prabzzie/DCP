﻿/**
 * @file
 *
 * @version 1.0
 *
 * @section DESCRIPTION
 *
 * Main entry point for dcp. This file is responsible for parsing all
 * arguments, command line and environment, and beginning an dcp call.
 *
 */

/* for asprintf */
#define _GNU_SOURCE
#include <stdio.h>
#undef _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>


#include "config.h"     /* generated by autotools */
#include "cmdline.h"    /* generated by gengetopt */
#include "digest.h"
#include "index/index.h"
#include "io/io.h"
#include "io_dcp_processor.h"
#include "logging.h"
#include "impl/dcp.h"


/* MACROS *********************************************************************/


/*
 * Environment Variable names dcp responds to
 */
#define ENV_OWNER           "DCP_OWNER"
#define ENV_GROUP           "DCP_GROUP"
#define ENV_CACHE_SIZE      "DCP_CACHE_SIZE"


/* Type Defs ******************************************************************/


/**
 * This structure contains all the options parsed from the command line and
 * environment.
 */
struct mainopts {
    const char **files;     /**< all the source paths specified.              */
    ssize_t filecount;      /**< number of source paths                       */
    const char *dest;       /**< path to where we are copying the source to   */
    int digests;            /**< mask of what digests dcp should calculate   */
    const char **inputs;    /**< result files from previous runs of dcp     */
    size_t inputcount;      /**< # input files specified                      */
    FILE *outputstream;     /**< where we should write results to             */
    char *outfilename;      /**< the output file that outputstream is writing */

    uid_t uid;              /**< id of who will own the copies                */
    gid_t gid;              /**< id of what group will own the copies         */
    char *username;         /**< who will own the copies                      */
    char *groupname;        /**< what group will own the copies               */

    size_t cache_size;      /**< how much memory to set aside for caching     */

    int verbose_mode;       /**< should we output what is being done          */
};


/* Private API ****************************************************************/


/**
 * writes information about this run to the specified output. The information
 * can be used to determine what options were used.
 */
static int print_metadata(FILE *out, const char *version, int argc,
        const char *argv[], const struct mainopts *opts, int digests);


/*
 * the following functions are used to translate options set via the environment
 * or the command line to usable values
 */
static int    parse_digests(const struct cmdline_info *info);
static FILE  *parse_outputstream(const struct cmdline_info *info,
        char **outfilename);
static gid_t  parse_group(const struct cmdline_info *info, char **name);
static uid_t  parse_owner(const struct cmdline_info *info, char **name);
static size_t parse_cache_size(const struct cmdline_info *info);

static index_t *build_index(int digests, const char *paths[], size_t count);

static int mainopts_parse(struct mainopts *opts,const struct cmdline_info*info);
static void mainopts_cleanup(struct mainopts *opts);

static int dcp_main(const struct mainopts *opts, int argc, const char *argv[]);

/**
 * for dcp we want `dcp src dest` to be the same as `dcp src dest/src` where
 * dest exists in both. To make this happen before we call dcp we will create
 * the directory so it appears we were called with the latter instead of the
 * former.
 */
static int prepare(const char **files, size_t count, const char *dest,
        char **newdest);


/* Main ***********************************************************************/


int main(int argc, const char *argv[])
{
    int r;
    struct cmdline_info info;
    struct mainopts opts;

    /* use the gengetopts code to parse the command line input, then convert to
     * a dcp_options struct */
    cmdline_parser(argc, (char **) argv, &info);
    mainopts_parse(&opts, &info);
    r = dcp_main(&opts, argc, argv);
    mainopts_cleanup(&opts);
    cmdline_parser_free(&info);
    return r;
}


/* Private Impl ***************************************************************/


size_t parse_cache_size(const struct cmdline_info *info)
{
    size_t size;
    const char *val;
    char *end;

    size = 32768; /* default if not specified */

    val = getenv(ENV_CACHE_SIZE);
    if (info->cache_size_given)
        val = info->cache_size_arg;

    if (val == NULL)
        return size;

    size = strtol(val, &end, 0);
    if (val == end)
        log_critx(EXIT_FAILURE, "invalid cache size: '%s'", val);

    switch (*end)
    {
    case '\0':                                              break;
    case 'k':  case 'K':    size *= 1024;                   break;
    case 'm':  case 'M':    size *= (1024 * 1024);          break;
    case 'g':  case 'G':    size *= (1024 * 1024 * 1024);   break;
    default:
        log_critx(EXIT_FAILURE, "invalid cache suffix: '%s'", val);
    }

    return size;
}


int parse_digests(const struct cmdline_info *info)
{
    int digests;

    digests = 0;
     /* create digest mask */
    if (info->all_flag)     return DGST_ALL;
    if (info->md5_flag)     digests  |=  DGST_MD5;
    if (info->sha1_flag)    digests  |=  DGST_SHA1;
    if (info->sha256_flag)  digests  |=  DGST_SHA256;
    if (info->sha512_flag)  digests  |=  DGST_SHA512;
    if (digests == 0)
        digests = DGST_MD5;
    return digests;
}


FILE *parse_outputstream(const struct cmdline_info *info, char **outfilename)
{
    int fd;
    size_t i;
    char name[32];  /* enough space to hold "dcp(1234567890).out\0" */
    FILE *stream;

    if (info->output_given)
    {
        if ((stream = fopen(info->output_arg, "w")) == NULL)
            log_crit(EXIT_FAILURE,"failed to open output file '%s'",
                    info->output_arg);
        *outfilename = strdup(info->output_arg);
    }

    /* if no output is specified create an dcp.out in cwd */
    else
    {
        snprintf(name, sizeof(name), "dcp.out");
        i = 0;
        while ((fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0666)) == -1)
        {
            if (errno == EEXIST)
                snprintf(name, sizeof(name), "dcp(%zu).out", ++i);

            else
                log_crit(EXIT_FAILURE, "cannot create output file '%s'", name);
        }

        if ((stream = fdopen(fd, "a")) == NULL)
            log_crit(EXIT_FAILURE, "cannot create output stream");
        *outfilename = strdup(name);
    }

    return stream;
}


uid_t parse_owner(const struct cmdline_info *info, char **name)
{
    const char *user;
    struct passwd *pwd;

    user = getenv(ENV_OWNER);
    if (info->owner_given)
        user = info->owner_arg;

    if (name != NULL)
        *name = user == NULL? NULL : strdup(user);

    /* if not set by command line or environment use euid */
    if (user == NULL)
        return geteuid();

    /* lookup uid for username, if no user default to */
    if ((pwd = getpwnam(user)) == NULL)
    {
        warnx("uid lookup for '%s' failed defaulting to euid", user);
        return geteuid();
    }

    return pwd->pw_uid;
}


gid_t parse_group(const struct cmdline_info *info, char **name)
{
    const char *groupname;
    struct group *grp;

    groupname = getenv(ENV_GROUP);
    if (info->group_given)
        groupname = info->group_arg;

    if (name != NULL)
        *name = groupname == NULL? NULL : strdup(groupname);

    /* if not set default to egid */
    if (groupname == NULL)
        return getegid();

    if ((grp = getgrnam(groupname)) == NULL)
    {
        warnx("gid lookup for '%s' failed defaulting to egid", groupname);
        return getegid();
    }

    return grp->gr_gid;
}


int mainopts_parse(struct mainopts *opts, const struct cmdline_info *info)
{
    /* initialize logging */
    logging_debug_mode = info->debug_flag;

    /* setup input files and output dir */
    opts->filecount     = ((signed) info->inputs_num) - 1;
    if (opts->filecount < 0)
        log_critx(EXIT_FAILURE, "missing file operand");
    if (opts->filecount == 0)
        log_critx(EXIT_FAILURE,
                "missing destination file operand after '%s'", info->inputs[0]);
    opts->files          = (const char **) info->inputs;
    opts->dest           = opts->files[opts->filecount];
    opts->digests        = parse_digests(info);
    opts->outputstream   = parse_outputstream(info, &opts->outfilename);
    opts->inputs         = (const char **) info->input_arg;
    opts->inputcount     = info->input_given;
    opts->uid            = parse_owner(info, &opts->username);
    opts->gid            = parse_group(info, &opts->groupname);
    opts->cache_size     = parse_cache_size(info);
    opts->verbose_mode   = info->verbose_flag;
    return 0;
}


void mainopts_cleanup(struct mainopts *opts)
{
    if (opts->outfilename  != NULL)     free(opts->outfilename);
    if (opts->username     != NULL)     free(opts->username);
    if (opts->groupname    != NULL)     free(opts->groupname);
    if (opts->outputstream != NULL)     fclose(opts->outputstream);
}


int dcp_main(const struct mainopts *opts, int argc, const char *argv[])
{
    int r;
    index_t *idx;
    int digests;
    io_dcp_processor_ctx_t *ctx;
    struct dcp_options dcpopts;
    char *dest;

    /* initilaize the index */
    idx = NULL;
    digests = opts->digests;
    if (opts->inputs != NULL)
    {
        /* when inputs are given generate their digests instead of relying on
         * the command line args */
        if (io_index_digest_peek(opts->inputs, opts->inputcount, &digests) != 0)
            log_critx(EXIT_FAILURE,
                    "cannot determine digest types from input file(s)");
        idx = build_index(digests, opts->inputs, opts->inputcount);
    }

    /* output information about this run of dcp */
    print_metadata(opts->outputstream, VERSION, argc, argv, opts, digests);

    /* setup how and where to send the data gathered during this run */
    if (io_dcp_processor_ctx_create(&ctx, opts->outputstream) == -1)
        log_critx(EXIT_FAILURE, "cannot instantiate output context");

    /* set the options struct */
    dcpopts.bufsize           = opts->cache_size;
    dcpopts.digests           = opts->digests;
    dcpopts.uid               = opts->uid;
    dcpopts.gid               = opts->gid;
    dcpopts.index             = idx;
    dcpopts.verbose           = opts->verbose_mode;

    /* quick check and dir creation if needed, will provide an updated dest
     * path if needed */
    if (prepare(opts->files, opts->filecount, opts->dest, &dest) != 0)
        log_critx(EXIT_FAILURE, "cannot prepare destination");

    /* if prepare was successful and didn't need to alter dest it sets dest to
     * NULL, point it to the original dest and continue */
    if (dest == NULL)
        dest = strdup(opts->dest);

    /* Start the copy */
    r = dcp(dest, opts->files, opts->filecount, &dcpopts,
            &io_dcp_processor, ctx);

    /* cleanup */
    free(dest);
    if (idx   != NULL) index_free(idx);

    io_dcp_processor_ctx_free(ctx);

    return r;
}


index_t *build_index(int digests, const char *paths[], size_t count)
{
    index_t *idx;
    digest_t type;
    size_t i;

	/* assign type to the first valid one we find, checking md5 then sha1 ... */
	if (  !(type = digests & DGST_MD5)    && !(type = digests & DGST_SHA1) &&
		  !(type = digests & DGST_SHA256) && !(type = digests & DGST_SHA512))
		log_critx(EXIT_FAILURE, "corrput parsing of digest types from inputs");

    if (index_create(&idx, type) != 0)
        log_critx(EXIT_FAILURE, "cannot create index");

    for (i = 0; i < count; i++)
        if (io_index_read(idx, paths[i]) != 0)
            log_critx(EXIT_FAILURE,
                    "error building index with entries from '%s'", paths[i]);

    return idx;
}

/*
 * ensure that the following 2 commands give the same dcp output
 *  dcp src dest
 *  dcp src dest/src
 *
 * where src and dest are directories that exist
 */
int prepare(const char **files, size_t count, const char *dest, char **newdest)
{
    int r;
    struct stat st;
    char *path;
    char *name;
    char *fulldest;

    *newdest = NULL;

    /* if we have a single src operand and it is a dir and a dest that exists
     * create the new directory and update the dest string */
    if (count == 1)
    {
        /* duplicate so we can use basename on it */
        path = strdup(files[0]);
        if (stat(path, &st) != 0)
        {
            log_error("cannot stat `%s'", path);
            free(path);
            return -1;
        }

        if (S_ISDIR(st.st_mode))
        {
            r = stat(dest, &st);
            /* if dest exists ... */
            if (r == 0)
            {
                /* ... and is a directory */
                if (S_ISDIR(st.st_mode))
                {
                    name = basename(path);
                    /* build the dest/name path including the slash if needed */
                    if(asprintf(&fulldest, "%s%s%s",
                            dest, dest[strlen(dest)-1] == '/'? "" : "/", name) < 0)
                    {
                        fulldest = NULL;
                    }
                    /* success report back the new dest */
                    *newdest = fulldest;
                }
            }

            /* dest not existing is not an error */
            else if(errno != ENOENT)
            {
                log_error("cannot stat `%s'", dest);
                free(path);
                return -1;
            }
        }
        free(path);
    }

    return 0;
}


int print_metadata(FILE *out, const char *version, int argc,
        const char *argv[], const struct mainopts *opts, int digests)
{
    time_t t;
    char *timestamp;
    char *dgsts[4];
    int dsize;
    char *cwd;
    char hostname[HOST_NAME_MAX + 1];

    if (out == NULL)
        return 0;

    /* generate current timestamp */
    t = time(NULL);
    timestamp = ctime(&t);
    timestamp[strlen(timestamp) - 1] = '\0'; /* remove the '\n' ctime appends */

    /* output what digests are valid */
    dsize = 0;
    if (HAS_MD5(digests))    dgsts[dsize++] = "md5";
    if (HAS_SHA1(digests))   dgsts[dsize++] = "sha1";
    if (HAS_SHA256(digests)) dgsts[dsize++] = "sha256";
    if (HAS_SHA512(digests)) dgsts[dsize++] = "sha512";

    /* current working directory */
    if ((cwd = getcwd(NULL, 0)) == NULL)
    	log_warn("cannot retrieve current working directory");

    /* hostname */
    gethostname(hostname, sizeof(hostname));

    /* send the metadata to output */
    io_metadata_put("File Generated by dcp DO NOT EDIT", NULL, out);
    io_metadata_put(     "version    ", version, out);
    io_metadata_put(     "timestamp  ", timestamp, out);
    io_metadata_put_strs("command    ", argc, argv, " ", out);
    io_metadata_put_strs("digests    ", dsize, (const char **) dgsts, ", ",out);
    io_metadata_put(     "host       ", hostname, out);

    if (cwd != NULL)
    {
    	io_metadata_put_json("cwd        ", 1, (const char **) &cwd, out);
    	free(cwd);
    }

    io_metadata_put_json("sources    ", opts->filecount, opts->files, out);
    io_metadata_put_json("destination", 1, (const char **) &opts->dest, out);
    io_metadata_put_json("output     ", 1,(const char**)&opts->outfilename,out);

    if (opts->username != NULL)
        io_metadata_put( "data_owner ", opts->username, out);

    if (opts->groupname != NULL)
        io_metadata_put( "data_group ", opts->groupname, out);

    return 0;
}

