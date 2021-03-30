#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <linux/limits.h>

typedef struct {
	const char* user;
	const char* host;
	const char* pwd;
	const char* cwd;
	int error;
} prompt_data;

static char* rev_parse[] = {"git", "rev-parse", "--git-dir", "--is-inside-git-dir", "--is-bare-repository", "--is-inside-work-tree", "--short", "HEAD", NULL};
static char* diff[] = {"git", "diff", "--no-ext-diff", "--quiet", NULL};
static char* diff_cached[] = {"git", "diff", "--no-ext-diff", "--quiet", "--cached", NULL};
static char* check_stash[] = {"git", "rev-parse", "--verify", "--quiet", "refs/stash", NULL};
static char* read_head[] = {"git", "symbolic-ref", "HEAD", NULL};
static char* describe[] = {"git", "describe", "--contains", "--all", "HEAD", NULL};
static char* upstream[] = {"git", "rev-list", "--count", "--left-right", "@{upstream}...HEAD", NULL};

static char prompt[4096];
static char* current = prompt;
static int remaining = sizeof prompt;
static const char* lastbg = NULL;

void append(const char* src, ...) {
	va_list ap;
	va_start(ap, src);
	while(src) {
		while (remaining && *src) {
			char c = *(src++);
			if (c == '$' || c == '\\') {
				*(current++) = '\\';
				--remaining;
			}
			*(current++) = c;
			--remaining;
		}
		src = va_arg(ap, char*);
	}
	va_end(ap);
}

void appendraw(const char* src, ...) {
	va_list ap;
	va_start(ap, src);
	while(src) {
		while (remaining && *src) {
			*(current++) = *(src++);
			--remaining;
		}
		src = va_arg(ap, char*);
	}
	va_end(ap);
}

void section(const char* fg, const char* bg) {
	if (lastbg) {
		appendraw(
			" ",
			"\\[\e[38;5;", lastbg, "m\e[48;5;", bg, "m\\]",
			"\ue0b0 ",
			"\\[\e[38;5;", fg, "m\\]",
			NULL
		);
	} else {
		appendraw("\\[\e[38;5;", fg, "m\e[48;5;", bg, "m\\]", NULL);
	}
	lastbg = bg;
}

const char* strcatv(char* dst, ...) {
	char* p = dst;
	char* src;
	va_list ap;
	va_start(ap, dst);
	src = va_arg(ap, char*);
	while (src) {
		char* s = src;
		while (*s) {
			*(p++) = *(s++);
		}
		src = va_arg(ap, char*);
	}
	*p = 0;
	va_end(ap);
	return dst;
}

int readp(char* const* cmd, int single, char* buf, size_t size) {
	int c2p[2], wstatus = 0;
	pid_t pid;
	char* p = buf;

	if (pipe(c2p) == 0 && (pid = fork()) != -1) {
		if (pid == 0) {
			// child
			close(c2p[0]);
			dup2(c2p[1], 1); // stdout
			dup2(open("/dev/null", O_WRONLY), 2); // stderr
			execvp(cmd[0], cmd);
			exit(1);
		} else {
			// parent
			int n, r = size - 1;
			close(c2p[1]);
			while (r > 0 && (n = read(c2p[0], p, r)) > 0) {
				p += n;
				r -= n;
			}
			close(c2p[0]);
			waitpid(pid, &wstatus, 0);
			if (single && p != buf) {
				--p;
			}
		}
	}

	*p = 0;
	return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
}

const char* readf(const char* path, char* buf, size_t size) {
	char* p = buf;
	int r = size - 1;
	int fd = open(path, O_RDONLY);

	if (fd != -1) {
		int n;
		while ((n = read(fd, p, r)) > 0) {
			p += n;
			r -= n;
		}
		close(fd);
		if (p != buf) {
			--p;
		}
	}

	*p = 0;
	return buf;
}

const char *split(char** next, char sep) {
	char *buf = *next;
	while (**next && **next != sep)
		++*next;
	**next = 0;
	++*next;
	return buf;
}

int isdir(const char* path) {
	struct stat statbuf;
	return lstat(path, &statbuf) == 0 && S_ISDIR(statbuf.st_mode);
}

int isreg(const char* path) {
	struct stat statbuf;
	return lstat(path, &statbuf) == 0 && S_ISREG(statbuf.st_mode);
}

int islnk(const char* path) {
	struct stat statbuf;
	return lstat(path, &statbuf) == 0 && S_ISLNK(statbuf.st_mode);
}

void title_section(const prompt_data* data) {
	appendraw("\\[\e]0;", NULL);
	append(data->user, "@", data->host, ":", data->cwd, NULL);
	appendraw("\a\\]", NULL);
}

void user_host_section(const prompt_data* data) {
	section("253", "242");
	append(data->user, "@", data->host, NULL);
}

void cwd_section(const prompt_data* data) {
	const char* dir = data->cwd;
	const char* sdir;
	int len = strlen(dir);
	if (len < 2) {
		sdir = dir;
	} else {
		sdir = dir + len - 2;
		while (sdir > dir && *(sdir - 1) != '/') {
			--sdir;
		}
	}

	section("15", "32");
	append(sdir, NULL);
}

void access_section(const prompt_data* data) {
	if (access(data->pwd, W_OK)) {
		section("254", "127");
		append("\ue0a2", NULL);
	}
}

void status_section(const prompt_data* data) {
	section(data->error ? "160" : "40", "0");
	append("$", NULL);
}

void ssh_section() {
	if (getenv("SSH_CLIENT")) {
		section("254", "172");
		append("\u26a1", NULL);
	}
}

void venv_section() {
	const char* venv = getenv("VIRTUAL_ENV");
	if (venv) {
		char tmp[PATH_MAX];
		strcpy(tmp, venv);
		section("0", "2");
		append("\U0001f40d", basename(tmp), NULL);
	}
}

void git_section() {
	char rpbuf[256];
	int status = readp(rev_parse, 0, rpbuf, sizeof rpbuf);
	if (*rpbuf != 0) {
		char tpath[PATH_MAX], tmp1[256], tmp2[256], tmp3[256], tmp4[256];
		char* next = rpbuf;
		const char* git = split(&next, '\n');
		const char* inside = split(&next, '\n');
		const char* bare = split(&next, '\n');
		const char* intree = split(&next, '\n');
		const char* ssha = status == 0 ? split(&next, '\n') : NULL;
		const char *r = NULL, *b = NULL, *w = NULL, *i = NULL, *s = NULL, *c = NULL, *p = NULL, *step = NULL, *total = NULL;
		int detached = 0;

		if (isdir(strcatv(tpath, git, "/rebase-merge", NULL))) {
			b = readf(strcatv(tpath, git, "/rebase-merge/head-name", NULL), tmp1, sizeof tmp1);
			step = readf(strcatv(tpath, git, "/rebase-merge/msgnum", NULL), tmp2, sizeof tmp2);
			total = readf(strcatv(tpath, git, "/rebase-merge/end", NULL), tmp3, sizeof tmp3);
		} else {
			if (isdir(strcatv(tpath, git, "/rebase-apply", NULL))) {
				step = readf(strcatv(tpath, git, "/rebase-apply/next", NULL), tmp2, sizeof tmp2);
				total = readf(strcatv(tpath, git, "/rebase-apply/last", NULL), tmp3, sizeof tmp3);
				if (isreg(strcatv(tpath, git, "/rebase-apply/rebasing", NULL))) {
					b = readf(strcatv(tpath, git, "/rebase-apply/head-name", NULL), tmp1, sizeof tmp1);
					r = "|REBASE";
				} else if (isreg(strcatv(tpath, git, "/rebase-apply/applying", NULL))) {
					r = "|AM";
				} else {
					r = "|AM/REBASE";
				}
			} else if (isreg(strcatv(tpath, git, "/MERGE_HEAD", NULL))) {
				r = "|MERGING";
			} else if (isreg(strcatv(tpath, git, "/CHERRY_PICK_HEAD", NULL))) {
				r = "|CHERRY-PICKING";
			} else if (isreg(strcatv(tpath, git, "/REVERT_HEAD", NULL))) {
				r = "|REVERTING";
			} else if (isreg(strcatv(tpath, git, "/BISECT_LOG", NULL))) {
				r = "|BISECTING";
			}

			if (!b) {
				if (islnk(strcatv(tpath, git, "/HEAD", NULL))) {
					// symlink symbolic ref
					if (readp(read_head, 1, tmp1, sizeof tmp1) == 0) {
						b = tmp1;
					}
				} else {
					const char* head = readf(strcatv(tpath, git, "/HEAD", NULL), tmp1, sizeof tmp1);
					// is it a symbolic ref?
					if (strncmp(head, "ref: ", 5) == 0) {
						b = head + 5;
					} else {
						detached = 1;
						*tmp1 = '(';
						if (readp(describe, 1, tmp1 + 1, sizeof tmp1 - 2) != 0) {
							strcatv(tmp1 + 1, ssha, "...", NULL);
						}
						int len = strlen(tmp1);
						*(tmp1 + len) = ')';
						*(tmp1 + len + 1) = 0;
						b = tmp1;
					}
				}
			}
		}

		if (b && strncmp(b, "refs/heads/", 11) == 0) {
			b += 11;
		}

		if (step && total) {
			r = strcatv(tmp4, r, " ", step, "/", total, NULL);
		}

		if (strcmp(inside, "true") == 0) {
			if (strcmp(bare, "true") == 0) {
				c = "BARE:";
			} else {
				b = "GIT_DIR!";
			}
		} else if (strcmp(intree, "true") == 0) {
			char tmp[256];
			if (readp(diff, 0, tmp, sizeof tmp) != 0) {
				w = "*";
			}
			if (readp(diff_cached, 0, tmp, sizeof tmp) != 0) {
				i = "+";
			} else if (!ssha) {
				i = "#";
			}
			if (readp(check_stash, 0, tmp, sizeof tmp) == 0) {
				s = "$";
			}
			if (readp(upstream, 1, tmp, sizeof tmp) == 0) {
				if (strcmp(tmp, "0\t0") == 0) {
					p = "";
				} else {
					next = tmp;
					const char* rmt = split(&next, '\t');
					const char* loc = split(&next, '\t');
					p = strcmp(rmt, "0") == 0 ? "\u2191" :
						strcmp(loc, "0") == 0 ? "\u2193" :
						"\u2195";
				}
			}
		}

		int dirty = detached || w || i || s;
		section(dirty ? "15" : "0", dirty ? "125" : "148");
		if (c) append(c, NULL);
		if (b) append(b, NULL);
		if (w || i || s) {
			append(" ", NULL);
			if (w) append(w, NULL);
			if (i) append(i, NULL);
			if (s) append(s, NULL);
		}
		if (r) append(r, NULL);
		if (p) append(p, NULL);
	}
}

void final_section() {
	appendraw("\\[\e[m\\] ", NULL);
}

int main(int argc, char** argv, char** envp) {
	prompt_data data;

	struct utsname name;
	uname(&name);

	data.user = getenv("USER");
	data.pwd = data.cwd = getenv("PWD");
	data.host = name.nodename;
	data.error = argc == 2 && strcmp(argv[1], "0") != 0;

	char tdir[PATH_MAX];
	const char* home = getenv("HOME");
	int homelen = strlen(home);
	if (strncmp(data.cwd, home, homelen) == 0) {
		*tdir = '~';
		strcpy(tdir + 1, data.cwd + homelen);
		data.cwd = tdir;
	}

	title_section(&data);
	user_host_section(&data);
	ssh_section();
	cwd_section(&data);
	access_section(&data);
	venv_section();
	git_section();
	status_section(&data);
	final_section();

	return write(1, prompt, sizeof prompt - remaining);
}
