#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/stat.h>

typedef unsigned long long ull_t;

#define PROCNAME_SIZE 16
#define PATH_TO_PROCINF_LEN 128

#define STAT_UTIME_POS 14
#define STAT_STIME_POS 15
#define STAT_STATE_POS 3

typedef struct {
        pid_t pid;

        char name[PROCNAME_SIZE];
        char state;

        double cpu;
        ull_t mem;

        uid_t uid;
} procinfo_t;

static int get_max(int count, ...)
{
		va_list ap;
        va_start(ap, count);

        int max = 0;
        
        for (int i = 0; i < count; i++) {
                int n = va_arg(ap, int);
                if (n > max) max = n;
        }

        va_end(ap);
		return max;
}

static int read_file(int fd, char *buffer, size_t bufsize)
{
        if (fd < 0 || !buffer || !bufsize) return -1;

        ssize_t read_bytes = 0;
        size_t total_bytes = 0;

        while (total_bytes < bufsize && (read_bytes = read(fd, buffer + total_bytes, bufsize - total_bytes)) > 0) {
                total_bytes += read_bytes;
        }

        if (read_bytes < 0) return -1;

        buffer[total_bytes] = '\0';

        return 0;
}

static void _Cpu_times_cp(const char *buffer, size_t *i, ull_t *time)
{
        const char *ptr = buffer + *i;
        size_t ccount = 0;

        while (isdigit(ptr[ccount])) ccount++;

        char tmp[PATH_TO_PROCINF_LEN];
        strncpy(tmp, ptr, ccount);
        tmp[ccount] = '\0';

        *i += ccount;

        *time = atoll(tmp);
}

static int _Get_cpu_times(pid_t pid, ull_t *utime, ull_t *stime)
{
        if (pid < 0 || !utime || !stime) return -1;

        char path[PATH_TO_PROCINF_LEN];
        sprintf(path, "/proc/%d/stat", pid);

        int fd = open(path, O_RDONLY);
        if (fd < 0) return -1;

        char buffer[4096];

        if (read_file(fd, buffer, sizeof(buffer)) != 0) {
                close(fd);
                return -1;
        }

        size_t pos  = 0;
        int in_word   = 0;
        int in_quotes = 0;

	const int last = get_max(2, STAT_UTIME_POS, STAT_STIME_POS);

        for (size_t i = 0; buffer[i]; i++) {
                if (isspace(buffer[i])) {
                        in_word = 0;
                }

                else if (buffer[i] == '\"' && !in_quotes) in_quotes = 1;
                else if (buffer[i] == '\"' && in_quotes) in_quotes = 0;

                else if (!in_word) {
                        in_word = 1;
                        pos++;

                        if (pos == STAT_UTIME_POS) {
                                _Cpu_times_cp(buffer, &i, utime);
                        }
                        else if (pos == STAT_STIME_POS) {
                                _Cpu_times_cp(buffer, &i, stime);

                                close(fd);
                                return 0;
                        }
                        else if (pos > last) {
                                break;
                        }
                }
        }

        close(fd);
        return -1;
}

static int pnf_getcpu(procinfo_t *info)
{
        if (!info) return -1;

        ull_t utime1;
        ull_t stime1;

        if (_Get_cpu_times(info->pid, &utime1, &stime1) != 0) {
                return -1;
        }

        usleep(100000);

        ull_t utime2;
        ull_t stime2;

        if (_Get_cpu_times(info->pid, &utime2, &stime2) != 0) {
                return -1;
        }

        ull_t ptd = (utime2 + stime2) - (utime1 + stime1);
        info->cpu = ((double)ptd / sysconf(_SC_CLK_TCK)) / 0.1 * 100.0;

        return 0;
}

static int pnf_getname(procinfo_t *info)
{
        if (!info) return -1;

        char path[PATH_TO_PROCINF_LEN];
        sprintf(path, "/proc/%d/comm", info->pid);

        int fd = open(path, O_RDONLY);
        if (fd < 0) return -1;

        if (read_file(fd, info->name, sizeof(info->name)) != 0) {
                close(fd);
                return -1;
        }

        char *nl = strrchr(info->name, '\n');
        if (nl) *nl = '\0';

        close(fd);
        return 0;
}

static int pnf_getstate(procinfo_t *info)
{
        if (!info) return -1;

        char path[PATH_TO_PROCINF_LEN];
        sprintf(path, "/proc/%d/stat", info->pid);

        int fd = open(path, O_RDONLY);
        if (fd < 0) return -1;

        char buffer[4096];

        if (read_file(fd, buffer, sizeof(buffer)) != 0) {
                close(fd);
                return -1;
        }

        size_t pos    = 0;
        int in_word   = 0;
        int in_quotes = 0;
	const int last      = STAT_STATE_POS;

        for (size_t i = 0; buffer[i]; i++) {
                if (isspace(buffer[i])) {
                        in_word = 0;
                }

                else if (buffer[i] == '\"' && !in_quotes) in_quotes = 1;
                else if (buffer[i] == '\"' && in_quotes) in_quotes = 0;

                else if (!in_word) {
                        in_word = 1;
                        pos++;

                        if (pos == STAT_STATE_POS) {
                                info->state = buffer[i];
                                close(fd);
                                return 0;
                        }

                        if (pos > last) break;
                }
        }

        close(fd);
        return -1;
}

static int pnf_getmem(procinfo_t *info)
{
        if (!info) return -1;

        char path[PATH_TO_PROCINF_LEN];
        sprintf(path, "/proc/%d/statm", info->pid);

        int fd = open(path, O_RDONLY);
        if (fd < 0) return -1;

        char buffer[4096];

        if (read_file(fd, buffer, sizeof(buffer)) != 0) {
                close(fd);
                return -1;
        }

        size_t words  = 0;
        int in_word   = 0;

        for (size_t i = 0; buffer[i]; i++) {
                if (isspace(buffer[i])) {
                        in_word = 0;
                }
                else if (!in_word) {
                        in_word = 1;
                        words++;

                        if (words == 2) {
                                char *ptr = buffer + i;
                                size_t ccount = 0;

                                while (isdigit(ptr[ccount])) ccount++;

                                char tmp[128];
                                strncpy(tmp, ptr, ccount);
                                tmp[ccount] = '\0';

                                ull_t rss = atoll(tmp);

                                info->mem = rss * (sysconf(_SC_PAGESIZE) / 1024);

                                close(fd);
                                return 0;
                        }
                        else if (words > 2) {
                                break;
                        }
                }
        }

        close(fd);
        return -1;
}

static int pnf_getuid(procinfo_t *info)
{
        if (!info) return -1;

        char path[PATH_TO_PROCINF_LEN];
        sprintf(path, "/proc/%d/status", info->pid);

        int fd = open(path, O_RDONLY);
        if (fd < 0) return -1;

        char buffer[8096 * 4];

        if (read_file(fd, buffer, sizeof(buffer)) != 0) {
                close(fd);
                return -1;
        }

        for (size_t i = 0, start = 0; buffer[i]; i++) {
                if (buffer[i] == '\n') {
                        if (strncmp(buffer + start, "Uid:", 4) == 0) {
                                char *ptr = buffer + start + 5;
                                while (!isdigit(*ptr)) ptr++;

                                size_t ccount = 0;
                                while (isdigit(ptr[ccount])) ccount++;

                                char tmp[128];
                                strncpy(tmp, ptr, ccount);
                                tmp[ccount] = '\0';

                                info->uid = atoi(tmp);

                                close(fd);
                                return 0;
                        }

                        start = i + 1;
                }
        }

        close(fd);
        return -1;
}

int pnf_update_info(procinfo_t *info, pid_t pid)
{
        if (!info || pid < 0) return -1;

        info->pid = pid;

        if (pnf_getcpu(info) != 0) return -1;
        if (pnf_getmem(info) != 0) return -1;
        if (pnf_getname(info) != 0) return -1;
        if (pnf_getstate(info) != 0) return -1;
        if (pnf_getuid(info) != 0) return -1;

        return 0;
}

void pnf_print(procinfo_t info)
{
        char *user = (info.uid == 0) ? "root" : "user";

        printf("    %-8d %-6s %-2c %-8.2f %-10llu %s\n",
                info.pid,
                user,
                info.state,
                info.cpu,
                info.mem,
                info.name);
}

static int str_isdigits(const char *str)
{
        if (!str) return 0;

        for (size_t i = 0; str[i]; i++) {
                if (!isdigit(str[i])) return 0;
        }

        return 1;
}

int proc_scanner(void)
{
        DIR *dir = opendir("/proc");
        if (!dir) return 1;

        struct dirent *entry;

        printf("\n    %-8s %-6s %-2s %-8s %-10s %s\n",
                "PID", "USER", "ST", "CPU%", "MEM(KB)", "NAME");

        printf("    %-8s %-6s %-2s %-8s %-10s %s\n",
                "--------", "------", "--", "--------", "----------", "------------------");


        while ((entry = readdir(dir))) {
                if (strcmp(entry->d_name, "..") == 0 || strcmp(entry->d_name, ".") == 0 || !str_isdigits(entry->d_name)) continue;

                struct stat st;
                char full_path[strlen(entry->d_name) + 64];
                sprintf(full_path, "/proc/%s", entry->d_name);

                if (lstat(full_path, &st) != 0) {
                        perror("lstat");
                        continue;
                }

                if (S_ISDIR(st.st_mode)) {
                        pid_t pid = atoll(entry->d_name);
                        procinfo_t info;

                        if (pnf_update_info(&info, pid) != 0) {
                                perror("get process info");
                                continue;
                        }

                        pnf_print(info);
                }
        }

        printf("\n");

        closedir(dir);
        return 0;
}

int main(void)
{
        return proc_scanner();
}
