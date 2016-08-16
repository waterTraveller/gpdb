#include <map>
#include <sstream>
#include <string>

#include "gpcheckcloud.h"

using std::map;
using std::string;
using std::stringstream;

volatile bool QueryCancelPending = false;

void printUsage(FILE *stream) {
    fprintf(stream,
            "Usage: gpcheckcloud -c \"s3://endpoint/bucket/prefix "
            "config=path_to_config_file\", to check the configuration.\n"
            "       gpcheckcloud -d \"s3://endpoint/bucket/prefix "
            "config=path_to_config_file\", to download and output to stdout.\n"
            "       gpcheckcloud -u \"s3://endpoint/bucket/prefix "
            "config=path_to_config_file\" -f \"/path/to/file\", to upload a file to S3.\n"
            "       gpcheckcloud -t, to show the config template.\n"
            "       gpcheckcloud -h, to show this help.\n");
}

// parse the arguments into char-string value pairs
map<char, string> parseCommandLineArgs(int argc, char *argv[]) {
    int opt = 0;
    map<char, string> optionPairs;

    while ((opt = getopt(argc, argv, "c:d:u:f:ht")) != -1) {
        switch (opt) {
            case 'c':
            case 'd':
            case 'u':
            case 'f':
            case 'h':
            case 't':
                if (optarg == NULL) {
                    optionPairs[opt] = "";
                } else if (optarg[0] == '-') {
                    fprintf(stderr, "Failed. Invalid argument for -%c: '%s'\n\n", opt, optarg);
                    printUsage(stderr);
                    exit(EXIT_FAILURE);
                } else {
                    optionPairs[opt] = optarg;
                }

                break;
            default:  // '?'
                printUsage(stderr);
                exit(EXIT_FAILURE);
        }
    }

    return optionPairs;
}

// check if command line arguments are valid
void validateCommandLineArgs(map<char, string> &optionPairs) {
    uint64_t count = optionPairs.count('f') + optionPairs.count('u');

    if ((count == 2) && (optionPairs.size() == 2)) {
        return;
    } else if (count == 1) {
        fprintf(stderr, "Failed. Option \'-u\' must work with \'-f\'\n\n");
        printUsage(stderr);
        exit(EXIT_FAILURE);
    }

    if (optionPairs.size() > 1) {
        stringstream ss;

        ss << "Failed. Can't set options ";

        // concatenate all option names
        // e.g. if we have -c and -d, insert "-c, -d" into the stream.
        for (map<char, string>::iterator i = optionPairs.begin(); i != optionPairs.end(); i++) {
            ss << "'-" << i->first << "' ";
        }

        ss << "at the same time.";

        // example message: "Failed. Can't set options '-c' '-d' at the same time."
        fprintf(stderr, "%s\n\n", ss.str().c_str());
        printUsage(stderr);
        exit(EXIT_FAILURE);
    }
}

void printTemplate() {
    printf(
        "[default]\n"
        "secret = \"aws secret\"\n"
        "accessid = \"aws access id\"\n"
        "threadnum = 4\n"
        "chunksize = 67108864\n"
        "low_speed_limit = 10240\n"
        "low_speed_time = 60\n"
        "encryption = true\n");
}

void printBucketContents(const ListBucketResult &result) {
    char urlbuf[256];
    vector<BucketContent>::const_iterator i;

    for (i = result.contents.begin(); i != result.contents.end(); i++) {
        snprintf(urlbuf, 256, "%s", i->getName().c_str());
        printf("File: %s, Size: %" PRIu64 "\n", urlbuf, i->getSize());
    }
}

bool checkConfig(const char *urlWithOptions) {
    if (!urlWithOptions) {
        return false;
    }

    GPReader *reader = reader_init(urlWithOptions);
    if (!reader) {
        return false;
    }

    ListBucketResult result = reader->getKeyList();

    if (result.contents.empty()) {
        fprintf(stderr,
                "\nYour configuration works well, however there is no file matching your "
                "prefix.\n");
    } else {
        printBucketContents(result);
        fprintf(stderr, "\nYour configuration works well.\n");
    }

    reader_cleanup(&reader);

    return true;
}

bool downloadS3(const char *urlWithOptions) {
    if (!urlWithOptions) {
        return false;
    }

    int data_len = BUF_SIZE;
    char data_buf[BUF_SIZE];
    bool ret = true;

    thread_setup();

    GPReader *reader = reader_init(urlWithOptions);
    if (!reader) {
        return false;
    }

    do {
        data_len = BUF_SIZE;

        if (!reader_transfer_data(reader, data_buf, data_len)) {
            fprintf(stderr, "Failed to read data from Amazon S3\n");
            ret = false;
            break;
        }

        fwrite(data_buf, (size_t)data_len, 1, stdout);
    } while (data_len);

    reader_cleanup(&reader);

    thread_cleanup();

    return ret;
}

bool uploadS3(const char *urlWithOptions, const char *fileToUpload) {
    if (!urlWithOptions) {
        return false;
    }

    size_t data_len = BUF_SIZE;
    char data_buf[BUF_SIZE];
    size_t read_len = 0;
    bool ret = true;

    thread_setup();

    GPWriter *writer = writer_init(urlWithOptions);
    if (!writer) {
        return false;
    }

    FILE *fd = fopen(fileToUpload, "r");
    if (fd == NULL) {
        fprintf(stderr, "File does not exist\n");
        ret = false;
    } else {
        do {
            read_len = fread(data_buf, 1, data_len, fd);

            if (read_len == 0) {
                break;
            }

            if (!writer_transfer_data(writer, data_buf, (int)read_len)) {
                fprintf(stderr, "Failed to write data to Amazon S3\n");
                ret = false;
                break;
            }
        } while (read_len == data_len);

        if (ferror(fd)) {
            ret = false;
        }

        fclose(fd);
    }

    writer_cleanup(&writer);

    thread_cleanup();

    return ret;
}

int main(int argc, char *argv[]) {
    bool ret = true;

    s3ext_loglevel = EXT_ERROR;
    s3ext_logtype = STDERR_LOG;

    if (argc == 1) {
        printUsage(stderr);
        exit(EXIT_FAILURE);
    }

    map<char, string> optionPairs = parseCommandLineArgs(argc, argv);

    validateCommandLineArgs(optionPairs);

    if (!optionPairs.empty()) {
        const char *arg = optionPairs.begin()->second.c_str();

        switch (optionPairs.begin()->first) {
            case 'c':
                ret = checkConfig(arg);
                break;
            case 'd':
                ret = downloadS3(arg);
                break;
            case 'u':
            case 'f':
                ret = uploadS3(optionPairs['u'].c_str(), optionPairs['f'].c_str());
                break;
            case 'h':
                printUsage(stdout);
                break;
            case 't':
                printTemplate();
                break;
            default:
                printUsage(stderr);
                exit(EXIT_FAILURE);
        }
    }

    if (ret) {
        exit(EXIT_SUCCESS);
    } else {
        fprintf(stderr, "Failed. Please check the arguments and configuration file.\n\n");
        printUsage(stderr);
        exit(EXIT_FAILURE);
    }
}
