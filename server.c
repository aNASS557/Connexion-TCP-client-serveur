#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "common.h"

#define MAX_CLIENTS 100
#define MAX_CHANNELS 20

struct client {
    int fd;                    /* -1 si la place est libre */
    struct sockaddr_in addr;
    char nick[NICK_LEN];
    int channel;               /* indice dans channels, -1 si aucun */
};

struct channel {
    int used;
    char name[INFOS_LEN];
};

struct client clients[MAX_CLIENTS];
struct channel channels[MAX_CHANNELS];

void die(int ret_value, const char *msg) {
    if (ret_value < 0) {
        perror(msg);
        exit(EXIT_FAILURE);
    }
}

/* Ici on ne quitte pas si un client part : on renvoie -1 */
static int read_full(int fd, void *ptr, int size) {
    int read_bytes = 0;
    int ret_value;
    while (read_bytes != size) {
        ret_value = read(fd, (char *)ptr + read_bytes, size - read_bytes);
        if (ret_value < 0 && errno == EINTR) continue;
        if (ret_value <= 0) return -1;
        read_bytes += ret_value;
    }
    return 0;
}

static int write_full(int fd, const void *ptr, int size) {
    int written_bytes = 0;
    int ret_value;
    while (written_bytes != size) {
        ret_value = write(fd, (const char *)ptr + written_bytes, size - written_bytes);
        if (ret_value < 0 && errno == EINTR) continue;
        if (ret_value <= 0) return -1;
        written_bytes += ret_value;
    }
    return 0;
}

static void send_frame(int fd, enum msg_type type, const char *sender,
                       const char *infos, const void *payload, int len) {
    struct message msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    msg.pld_len = len;
    strncpy(msg.nick_sender, sender, NICK_LEN - 1);
    strncpy(msg.infos, infos, INFOS_LEN - 1);
    if (write_full(fd, &msg, sizeof(msg)) < 0) return;
    if (len > 0) write_full(fd, payload, len);
}

static void send_text(int fd, const char *sender, const char *text) {
    send_frame(fd, ECHO_SEND, sender, "", text, strlen(text));
}

static int receive_frame(int fd, struct message *msg, char *payload) {
    if (read_full(fd, msg, sizeof(*msg)) < 0) return -1;
    if (msg->pld_len < 0 || msg->pld_len >= MSG_LEN) return -1;
    if (msg->pld_len > 0 && read_full(fd, payload, msg->pld_len) < 0) return -1;
    payload[msg->pld_len] = '\0';
    msg->nick_sender[NICK_LEN - 1] = '\0';
    msg->infos[INFOS_LEN - 1] = '\0';
    return 0;
}

/* écrit n en texte dans s (pas de sprintf) */
static void number_to_text(int n, char *s) {
    char tmp[12];
    int len = 0, k = 0;
    if (n == 0) tmp[len++] = '0';
    while (n > 0) {
        tmp[len++] = '0' + n % 10;
        n = n / 10;
    }
    while (len > 0) s[k++] = tmp[--len];
    s[k] = '\0';
}

/* lettres et chiffres uniquement, longueur < max */
static int valid_name(const char *name, int max) {
    int i, len = strlen(name);
    if (len == 0 || len >= max) return 0;
    for (i = 0; i < len; i++)
        if (!isalnum((unsigned char)name[i])) return 0;
    return 1;
}

static int find_client(const char *name) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd != -1 && clients[i].nick[0] != '\0' &&
            strcmp(clients[i].nick, name) == 0)
            return i;
    return -1;
}

static int find_channel(const char *name) {
    int i;
    for (i = 0; i < MAX_CHANNELS; i++)
        if (channels[i].used && strcmp(channels[i].name, name) == 0)
            return i;
    return -1;
}

static int count_members(int ch) {
    int i, count = 0;
    for (i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd != -1 && clients[i].channel == ch) count++;
    return count;
}

/* envoie un texte à tous les membres du salon sauf except */
static void notify_channel(int ch, int except, const char *text) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++)
        if (i != except && clients[i].fd != -1 && clients[i].channel == ch)
            send_text(clients[i].fd, "INFO", text);
}

static void leave_channel(int i) {
    int ch = clients[i].channel;
    char text[MSG_LEN];

    if (ch == -1) return;
    strcpy(text, clients[i].nick);
    strcat(text, " has quit ");
    strcat(text, channels[ch].name);
    notify_channel(ch, i, text);
    clients[i].channel = -1;

    if (count_members(ch) == 0) {
        strcpy(text, "You were the last user in this channel, ");
        strcat(text, channels[ch].name);
        strcat(text, " has been destroyed");
        send_text(clients[i].fd, "INFO", text);
        channels[ch].used = 0;
    }
}

static void join_channel(int i, int ch) {
    char text[MSG_LEN];

    if (clients[i].channel == ch) {
        strcpy(text, "You have already joined ");
        strcat(text, channels[ch].name);
        send_text(clients[i].fd, "INFO", text);
        return;
    }
    leave_channel(i);
    clients[i].channel = ch;

    strcpy(text, "You have joined ");
    strcat(text, channels[ch].name);
    send_text(clients[i].fd, "INFO", text);
    /* ce message dit au client dans quel salon il est maintenant */
    send_frame(clients[i].fd, MULTICAST_JOIN, "", channels[ch].name, NULL, 0);

    strcpy(text, clients[i].nick);
    strcat(text, " has joined ");
    strcat(text, channels[ch].name);
    notify_channel(ch, i, text);
}

static void remove_client(int i) {
    leave_channel(i);
    close(clients[i].fd);
    clients[i].fd = -1;
    clients[i].nick[0] = '\0';
}

static void handle_client(int i) {
    struct message msg;
    char payload[MSG_LEN];
    char text[MSG_LEN];
    char number[12];
    int fd = clients[i].fd;
    int j, ch;

    if (receive_frame(fd, &msg, payload) < 0) {
        remove_client(i);
        return;
    }

    if (msg.type == NICKNAME_NEW) {
        j = find_client(msg.infos);
        if (!valid_name(msg.infos, NICK_LEN))
            send_text(fd, "Serveur", "Pseudo invalide.");
        else if (j != -1 && j != i)
            send_text(fd, "Serveur", "Pseudo déjà utilisé.");
        else {
            strcpy(clients[i].nick, msg.infos);
            send_text(fd, "Serveur", "Bienvenue sur le chat !");
        }
        return;
    }
    if (clients[i].nick[0] == '\0') {
        send_text(fd, "Serveur", "Identifiez-vous avec /nick pseudo.");
        return;
    }

    if (msg.type == NICKNAME_LIST) {
        strcpy(text, "Online users are");
        for (j = 0; j < MAX_CLIENTS; j++) {
            if (clients[j].fd != -1 && clients[j].nick[0] != '\0' &&
                strlen(text) + strlen(clients[j].nick) + 5 < MSG_LEN) {
                strcat(text, "\n - ");
                strcat(text, clients[j].nick);
            }
        }
        send_text(fd, "Serveur", text);

    } else if (msg.type == NICKNAME_INFOS) {
        j = find_client(msg.infos);
        if (j == -1) {
            send_text(fd, "Serveur", "Utilisateur introuvable.");
        } else {
            number_to_text(ntohs(clients[j].addr.sin_port), number);
            strcpy(text, clients[j].nick);
            strcat(text, " : IP ");
            strcat(text, inet_ntoa(clients[j].addr.sin_addr));
            strcat(text, ", port ");
            strcat(text, number);
            send_text(fd, "Serveur", text);
        }

    } else if (msg.type == BROADCAST_SEND) {
        for (j = 0; j < MAX_CLIENTS; j++)
            if (j != i && clients[j].fd != -1 && clients[j].nick[0] != '\0')
                send_frame(clients[j].fd, ECHO_SEND, clients[i].nick, "", payload, msg.pld_len);

    } else if (msg.type == UNICAST_SEND) {
        j = find_client(msg.infos);
        if (j == -1)
            send_text(fd, "Serveur", "Destinataire introuvable.");
        else
            send_frame(clients[j].fd, ECHO_SEND, clients[i].nick, "", payload, msg.pld_len);

    } else if (msg.type == ECHO_SEND) {
        send_frame(fd, ECHO_SEND, clients[i].nick, "", payload, msg.pld_len);

    } else if (msg.type == MULTICAST_CREATE) {
        if (!valid_name(msg.infos, INFOS_LEN)) {
            send_text(fd, "Serveur", "Nom de salon invalide : lettres et chiffres uniquement.");
        } else if (find_channel(msg.infos) != -1) {
            send_text(fd, "Serveur", "Erreur : ce salon existe déjà.");
        } else {
            ch = 0;
            while (ch < MAX_CHANNELS && channels[ch].used) ch++;
            if (ch == MAX_CHANNELS) {
                send_text(fd, "Serveur", "Erreur : trop de salons.");
            } else {
                channels[ch].used = 1;
                strcpy(channels[ch].name, msg.infos);
                strcpy(text, "You have created channel ");
                strcat(text, channels[ch].name);
                send_text(fd, "INFO", text);
                join_channel(i, ch);
            }
        }

    } else if (msg.type == MULTICAST_LIST) {
        strcpy(text, "Salons disponibles :");
        for (ch = 0; ch < MAX_CHANNELS; ch++) {
            if (channels[ch].used && strlen(text) + strlen(channels[ch].name) + 4 < MSG_LEN) {
                strcat(text, "\n - ");
                strcat(text, channels[ch].name);
            }
        }
        if (strcmp(text, "Salons disponibles :") == 0)
            strcat(text, "\nAucun salon.");
        send_text(fd, "INFO", text);

    } else if (msg.type == MULTICAST_JOIN) {
        ch = find_channel(msg.infos);
        if (ch == -1)
            send_text(fd, "Serveur", "Erreur : salon introuvable.");
        else
            join_channel(i, ch);

    } else if (msg.type == MULTICAST_QUIT) {
        ch = clients[i].channel;
        if (ch == -1)
            send_text(fd, "Serveur", "Vous n'êtes dans aucun salon.");
        else if (msg.infos[0] != '\0' && strcmp(msg.infos, channels[ch].name) != 0)
            send_text(fd, "Serveur", "Vous n'êtes pas dans ce salon.");
        else
            leave_channel(i);

    } else if (msg.type == MULTICAST_SEND) {
        ch = clients[i].channel;
        if (ch == -1 || strcmp(msg.infos, channels[ch].name) != 0) {
            send_text(fd, "Serveur", "Rejoignez un salon pour envoyer un message.");
        } else if (strlen(clients[i].nick) + 4 + msg.pld_len < MSG_LEN) {
            strcpy(text, clients[i].nick);
            strcat(text, "> : ");
            strcat(text, payload);
            for (j = 0; j < MAX_CLIENTS; j++)
                if (j != i && clients[j].fd != -1 && clients[j].channel == ch)
                    send_text(clients[j].fd, "", text);
        }

    } else if (msg.type == FILE_REQUEST) {
        j = find_client(msg.infos);
        if (j == -1)
            send_text(fd, "Serveur", "Utilisateur introuvable.");
        else if (j == i)
            send_text(fd, "Serveur", "Auto-envoi impossible.");
        else
            send_frame(clients[j].fd, FILE_REQUEST, clients[i].nick, clients[j].nick, payload, msg.pld_len);

    } else if (msg.type == FILE_ACCEPT || msg.type == FILE_REJECT) {
        j = find_client(msg.infos);
        if (j != -1)
            send_frame(clients[j].fd, msg.type, clients[i].nick, clients[j].nick, payload, msg.pld_len);
    }
}

int main(int argc, char **argv) {
    const char *port = SERV_PORT;
    int server_fd, newfd, i, ret, yes = 1;
    struct sockaddr_in addr;
    socklen_t len;
    struct pollfd fds[MAX_CLIENTS + 1];

    if (argc == 2) port = argv[1];
    signal(SIGPIPE, SIG_IGN);   /* un client parti ne doit pas tuer le serveur */
    for (i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].channel = -1;
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    die(server_fd, "socket");
    die(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)), "setsockopt");
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(atoi(port));
    die(bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)), "bind");
    die(listen(server_fd, 10), "listen");
    printf("Serveur en écoute sur le port %s\n", port);

    while (1) {
        fds[0].fd = server_fd;
        fds[0].events = POLLIN;
        for (i = 0; i < MAX_CLIENTS; i++) {
            fds[i + 1].fd = clients[i].fd;   /* poll ignore les fd négatifs */
            fds[i + 1].events = POLLIN;
        }
        ret = poll(fds, MAX_CLIENTS + 1, -1);
        if (ret < 0 && errno == EINTR) continue;
        die(ret, "poll");

        if (fds[0].revents & POLLIN) {
            len = sizeof(addr);
            newfd = accept(server_fd, (struct sockaddr *)&addr, &len);
            die(newfd, "accept");
            i = 0;
            while (i < MAX_CLIENTS && clients[i].fd != -1) i++;
            if (i == MAX_CLIENTS) {
                close(newfd);
            } else {
                clients[i].fd = newfd;
                clients[i].addr = addr;
                clients[i].nick[0] = '\0';
                clients[i].channel = -1;
            }
        }
        for (i = 0; i < MAX_CLIENTS; i++)
            if (fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR))
                handle_client(i);
    }
    return 0;
}
