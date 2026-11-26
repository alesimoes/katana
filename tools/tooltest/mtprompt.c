/* Programa de teste do terminal do Bash (tools/tooltest/sendinput.sh): imita uma ferramenta de varias threads que pergunta um
 * caminho sem quebra de linha. A thread principal fica em sigsuspend, uma em futex (pthread_cond_wait), uma em poll com prazo
 * e a leitora em epoll_wait no stdin; so depois do epoll ela le a resposta. Nenhuma thread fica parada em read() no terminal,
 * entao a deteccao depende do texto da ultima linha.
 * Uso: mtprompt [texto da pergunta]   (padrao: "Path to Tool.xip: ")
 * Compilar: cc -O1 -pthread tools/tooltest/mtprompt.c -o mtprompt */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <sys/epoll.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static const char *prompt = "Path to Tool.xip: ";

static void *th_futex(void *a) { (void)a; pthread_mutex_lock(&mu); for (;;) pthread_cond_wait(&cv, &mu); return NULL; }
static void *th_poll(void *a) { (void)a; for (;;) poll(NULL, 0, 60000); return NULL; }
static void *th_reader(void *a) {
    (void)a;
    printf("Logged in as test-user.\nPlease download Tool from the vendor site and enter the path to the downloaded Tool.xip.\n\n%s", prompt);
    fflush(stdout);
    const int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = 0 };
    epoll_ctl(ep, EPOLL_CTL_ADD, 0, &ev);
    struct epoll_event got;
    while (epoll_wait(ep, &got, 1, -1) < 1) {}
    char b[1024];
    const ssize_t k = read(0, b, sizeof b - 1);
    b[k > 0 ? k : 0] = 0;
    b[strcspn(b, "\r\n")] = 0;
    printf("got path: %s\n", b);
    fflush(stdout);
    _exit(b[0] ? 0 : 3);
}
int main(int argc, char **argv) {
    if (argc > 1) prompt = argv[1];
    sigset_t all, none;
    sigfillset(&all);
    sigdelset(&all, SIGINT); sigdelset(&all, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    pthread_t t;
    pthread_create(&t, NULL, th_futex, NULL);
    pthread_create(&t, NULL, th_poll, NULL);
    pthread_create(&t, NULL, th_reader, NULL);
    sigemptyset(&none);
    sigaddset(&none, SIGCHLD);
    for (;;) sigsuspend(&none);
}
